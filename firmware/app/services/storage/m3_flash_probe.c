/*
 * W25Q128 分层探测实现。仅在 M3_ENABLE_FLASH_PROBE=ON 时参与编译。
 * 用法和六段输出含义见 m3_flash_probe.h。
 *
 * 背景（答辩和以后换板都用得上）：
 *   底板用 3 片单刀双掷模拟开关（U31/U33/U34）把 SPI2 的 SCK/MOSI/MISO
 *   在 W25Q128 与板载 ES8388 音频之间分流。切换信号 SPI_OR_I2S_SWITCH
 *   由八路拨码开关 BIT3 和 PCA9555(0x20) 的 IO0_2 共同驱动，
 *   官方文档说明：软件输出优先级高于手动拨码。
 *
 *   所以"读不到 Flash"至少有四种可能，必须分层排除：
 *     1. SPI2 外设自身没初始化 / 状态机卡死      → D0
 *     2. 模拟开关被切到 I2S 侧 / 拨码没生效      → D1、D4、D5
 *     3. Flash 处在深掉电(DPD)，只认 0xAB        → D2
 *     4. 命令与接收分两次 HAL 调用导致时钟错位    → D3
 */

#include "m3_flash_probe.h"

#include <stdio.h>
#include <stdint.h>

#include "main.h"       /* SPI_FLASH_CS_Pin / SPI_FLASH_CS_GPIO_Port */
#include "spi.h"        /* hspi2 */
#include "i2c.h"        /* hi2c1 */

extern SPI_HandleTypeDef hspi2;
extern I2C_HandleTypeDef hi2c1;

#define DIAG_PCA_ADDR_W   ((uint8_t)(0x20 << 1))  /* 7bit 0x20 → 8bit 写地址 0x40 */
#define DIAG_PCA_IN0      0x00                    /* Input Port 0  */
#define DIAG_PCA_OUT0     0x02                    /* Output Port 0 */
#define DIAG_PCA_CFG0     0x06                    /* Config Port 0 : 1=输入 0=输出 */
#define DIAG_SPI_SW_BIT   2                       /* BIT3 → IO0_2 → bit2 */

/*
 * 强制切到 I2S 侧时 MISO 悬空，可能让 SPI 状态机卡在 BUSY。
 * HAL 在非 READY 状态下会直接拒绝 TransmitReceive，rx 保持全 0，
 * 看起来像"读不到数据"（[D5] 曾经打出 000000 就是这个原因，
 * 而不是真的没切换成功）。
 */
static void diag_spi_recover(void)
{
    if (hspi2.State != HAL_SPI_STATE_READY) {
        (void)HAL_SPI_Abort(&hspi2);
    }
}

static void diag_cs_low(void)
{
    HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_RESET);
}

static void diag_cs_high(void)
{
    /* 与驱动 flash_cs_high() 保持一致的两个动作，缺一不可：
     *   1) 先等 SPI 的 BSY 清零，否则移位寄存器里最后一字节被 CS 截断；
     *   2) 拉高后留死区，满足 W25Q128 的 tCSW(>=50ns)，否则 Flash 不认为
     *      上一个命令结束，下一条多字节命令会被当成延续而丢弃。
     * 少了这两步，PROBE 读到的 JEDEC 会整体错位 —— [D3] 01 EF 40 30 就是
     * 0x18 左移一位变成 0x30 的结果，自检也随之误判成 jedec=EF0060。 */
    uint32_t t0 = HAL_GetTick();
    while ((hspi2.Instance->SR & SPI_SR_BSY) != 0) {
        if ((HAL_GetTick() - t0) > 10) break;
    }
    HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_SET);
    HAL_Delay(1);
}

/* 单窗口读 JEDEC：0x9F + 3 字节读，全程 CS 保持低、SPI 不停时钟 */
static uint32_t diag_jedec_single(void)
{
    uint8_t tx[4] = { 0x9F, 0xFF, 0xFF, 0xFF };
    uint8_t rx[4] = { 0, 0, 0, 0 };
    uint32_t id;

    diag_spi_recover();

    diag_cs_low();
    (void)HAL_SPI_TransmitReceive(&hspi2, tx, rx, 4, 100);
    diag_cs_high();

    printf("[D3] raw = %02X %02X %02X %02X\r\n",
           (unsigned)rx[0], (unsigned)rx[1], (unsigned)rx[2], (unsigned)rx[3]);

    id = ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];
    return id;
}

/* 0xAB 释放深掉电：W25Q128 处于 DPD 时，除 0xAB 外所有命令都不响应 */
static void diag_release_dpd(void)
{
    uint8_t tx = 0xAB, rx = 0;

    diag_spi_recover();
    diag_cs_low();
    (void)HAL_SPI_TransmitReceive(&hspi2, &tx, &rx, 1, 100);
    diag_cs_high();

    HAL_Delay(1);   /* tRES1 典型 3us，给 1ms 富余 */
}

/* 读状态寄存器 SR1 */
static uint8_t diag_read_sr1(void)
{
    uint8_t tx[2] = { 0x05, 0xFF };
    uint8_t rx[2] = { 0, 0 };

    diag_spi_recover();
    diag_cs_low();
    (void)HAL_SPI_TransmitReceive(&hspi2, tx, rx, 2, 100);
    diag_cs_high();

    return rx[1];
}

/* 读 PCA9555：拨码电平 / 配置方向 / 输出锁存 */
static HAL_StatusTypeDef diag_pca_read(uint8_t *in0, uint8_t *cfg0, uint8_t *out0)
{
    HAL_StatusTypeDef a, b, c;

    a = HAL_I2C_Mem_Read(&hi2c1, DIAG_PCA_ADDR_W, DIAG_PCA_IN0,  I2C_MEMADD_SIZE_8BIT, in0,  1, 50);
    b = HAL_I2C_Mem_Read(&hi2c1, DIAG_PCA_ADDR_W, DIAG_PCA_CFG0, I2C_MEMADD_SIZE_8BIT, cfg0, 1, 50);
    c = HAL_I2C_Mem_Read(&hi2c1, DIAG_PCA_ADDR_W, DIAG_PCA_OUT0, I2C_MEMADD_SIZE_8BIT, out0, 1, 50);

    return (a == HAL_OK && b == HAL_OK && c == HAL_OK) ? HAL_OK : HAL_ERROR;
}

/* 软件强制 IO0_2 为指定电平并设为输出（官方：软件优先级高于拨码） */
static HAL_StatusTypeDef diag_pca_force(uint8_t level)
{
    uint8_t cfg, out;

    if (HAL_I2C_Mem_Read(&hi2c1, DIAG_PCA_ADDR_W, DIAG_PCA_CFG0,
                         I2C_MEMADD_SIZE_8BIT, &cfg, 1, 50) != HAL_OK)
        return HAL_ERROR;
    if (HAL_I2C_Mem_Read(&hi2c1, DIAG_PCA_ADDR_W, DIAG_PCA_OUT0,
                         I2C_MEMADD_SIZE_8BIT, &out, 1, 50) != HAL_OK)
        return HAL_ERROR;

    /* 先写输出锁存值，再切输出方向，避免切换瞬间的毛刺 */
    if (level) out |=  (uint8_t)(1u << DIAG_SPI_SW_BIT);
    else       out &= (uint8_t)~(1u << DIAG_SPI_SW_BIT);

    if (HAL_I2C_Mem_Write(&hi2c1, DIAG_PCA_ADDR_W, DIAG_PCA_OUT0,
                          I2C_MEMADD_SIZE_8BIT, &out, 1, 50) != HAL_OK)
        return HAL_ERROR;

    cfg &= (uint8_t)~(1u << DIAG_SPI_SW_BIT);
    return HAL_I2C_Mem_Write(&hi2c1, DIAG_PCA_ADDR_W, DIAG_PCA_CFG0,
                             I2C_MEMADD_SIZE_8BIT, &cfg, 1, 50);
}

void m3_flash_probe_run(void)
{
    uint8_t in0 = 0, cfg0 = 0, out0 = 0, sr1 = 0;
    uint32_t id = 0;

    printf("\r\n----- SPI FLASH PROBE -----\r\n");

    /* D0：SPI2 外设自身状态 */
    printf("[D0] hspi2 State=0x%02X Err=0x%02X Presc=%lu CPOL=%lu CPHA=%lu\r\n",
           (unsigned)hspi2.State, (unsigned)hspi2.ErrorCode,
           (unsigned long)hspi2.Init.BaudRatePrescaler,
           (unsigned long)hspi2.Init.CLKPolarity,
           (unsigned long)hspi2.Init.CLKPhase);

    /* D1：PCA9555 在不在？拨码开关现在实际是什么电平？ */
    if (diag_pca_read(&in0, &cfg0, &out0) == HAL_OK) {
        printf("[D1] PCA9555 in0=0x%02X cfg0=0x%02X out0=0x%02X\r\n",
               (unsigned)in0, (unsigned)cfg0, (unsigned)out0);
        printf("[D1] BIT1..8 = %d%d%d%d%d%d%d%d  BIT3=%d\r\n",
               (in0 >> 0) & 1, (in0 >> 1) & 1, (in0 >> 2) & 1, (in0 >> 3) & 1,
               (in0 >> 4) & 1, (in0 >> 5) & 1, (in0 >> 6) & 1, (in0 >> 7) & 1,
               (in0 >> DIAG_SPI_SW_BIT) & 1);
    } else {
        printf("[D1] PCA9555 NO ACK (7bit addr 0x20)\r\n");
    }

    /* D2：退出深掉电，随后读 SR1 */
    diag_release_dpd();
    sr1 = diag_read_sr1();
    printf("[D2] after 0xAB: SR1=0x%02X (BUSY=%d)\r\n",
           (unsigned)sr1, (int)(sr1 & 0x01));

    /* D3：不动切换开关，单窗口读（用于排除拆分调用的时序错位） */
    id = diag_jedec_single();
    printf("[D3] JEDEC(single-window) = %06lX\r\n", (unsigned long)id);

    /* D4：软件强制 HIGH */
    if (diag_pca_force(1) == HAL_OK) {
        HAL_Delay(2);
        id = diag_jedec_single();
        printf("[D4] force BIT3=HIGH -> %06lX\r\n", (unsigned long)id);
    } else {
        printf("[D4] force HIGH failed (I2C write)\r\n");
    }

    /* D5：软件强制 LOW */
    if (diag_pca_force(0) == HAL_OK) {
        HAL_Delay(2);
        id = diag_jedec_single();
        printf("[D5] force BIT3=LOW  -> %06lX\r\n", (unsigned long)id);
    } else {
        printf("[D5] force LOW failed (I2C write)\r\n");
    }

    /* probe 结束后把 BIT3 强制到 SPI 侧并保持输出模式。
     *
     * ⚠️ 待实测确认：注释里"LOW = SPI 侧接通"是从原理图推断的，
     *    还没有用 D4/D5 两个读数验证过。如果实测发现 HIGH 才是 SPI 侧，
     *    这里的参数要改成 diag_pca_force(1)，否则探测跑完反而把
     *    SPI2 切到 I2S 侧，后面的自检必然全部失败。 */
    diag_pca_force(0);

    printf("----- PROBE END -----\r\n\r\n");
}
