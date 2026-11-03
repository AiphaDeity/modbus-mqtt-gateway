/*
 * spi_flash_w25q128.c — W25Q128 SPI Flash 驱动实现
 *
 * 硬件：SPI2，CS = PE4
 * 依赖：CubeMX 已生成 MX_SPI2_Init()，全局句柄 hspi2
 *       main.h 中已定义 SPI_FLASH_CS_Pin / SPI_FLASH_CS_GPIO_Port
 *
 * W25Q128 命令集：
 *   写使能        0x06
 *   读状态寄存器  0x05   （bit0 = BUSY，1=忙碌）
 *   读 JEDEC ID   0x9F   （返回 3 字节，厂家 0xEF，W25Q128 = 0x4018）
 *   读数据        0x03   （+ 3 字节地址）
 *   页编程        0x02   （+ 3 字节地址 + 数据，每页最多 256B）
 *   扇区擦除 4KB  0x20   （+ 3 字节地址）
 */

#include "spi_flash_w25q128.h"
#include "stm32f4xx_hal_spi.h"
#include "main.h"   // 取 SPI_FLASH_CS_Pin / SPI_FLASH_CS_GPIO_Port

extern SPI_HandleTypeDef hspi2;

/* ---------- 内部工具 ---------- */

static void flash_cs_low(void)  { HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_RESET); }
static void flash_cs_high(void) { HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_SET); }

// 发一个命令字节
static HAL_StatusTypeDef flash_send_cmd(uint8_t cmd)
{
    return HAL_SPI_Transmit(&hspi2, &cmd, 1, 100);
}

// 发 3 字节地址（W25Q128 用 24 位地址）
static HAL_StatusTypeDef flash_send_addr(uint32_t addr)
{
    uint8_t buf[3];
    buf[0] = (uint8_t)((addr >> 16) & 0xFF);
    buf[1] = (uint8_t)((addr >> 8)  & 0xFF);
    buf[2] = (uint8_t)( addr        & 0xFF);
    return HAL_SPI_Transmit(&hspi2, buf, 3, 100);
}

// 等待写入/擦除完成（轮询状态寄存器 bit0）
HAL_StatusTypeDef spi_flash_w25q128_wait_ready(uint32_t timeout_ms)
{
    uint8_t status;
    uint32_t start = HAL_GetTick();
    do {
        flash_cs_low();
        flash_send_cmd(0x05);                 // 读状态寄存器
        HAL_SPI_Receive(&hspi2, &status, 1, 100);
        flash_cs_high();
        if ((status & 0x01) == 0)             // bit0 = 0 表示空闲
            return HAL_OK;
    } while ((HAL_GetTick() - start) < timeout_ms);
    return HAL_TIMEOUT;
}

/* ---------- 对外 API ---------- */

HAL_StatusTypeDef spi_flash_w25q128_init(void)
{
    // 读 JEDEC ID，确认芯片在线且型号正确
    uint32_t id = spi_flash_w25q128_read_id();
    // W25Q128 的 JEDEC ID = 0xEF4018，只判断厂家字节 0xEF 即可
    if ((id & 0x0000FF) != 0xEF)
        return HAL_ERROR;
    return HAL_OK;
}

uint32_t spi_flash_w25q128_read_id(void)
{
    uint8_t buf[3] = {0, 0, 0};
    flash_cs_low();
    flash_send_cmd(0x9F);                    // JEDEC ID 命令
    HAL_SPI_Receive(&hspi2, buf, 3, 100);   // 读 3 字节
    flash_cs_high();
    return ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
}

HAL_StatusTypeDef spi_flash_w25q128_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0)
        return HAL_ERROR;

    HAL_StatusTypeDef st;
    flash_cs_low();
    flash_send_cmd(0x03);                    // 读数据命令
    flash_send_addr(addr);
    st = HAL_SPI_Receive(&hspi2, buf, len, 1000);
    flash_cs_high();
    return st;
}

HAL_StatusTypeDef spi_flash_w25q128_sector_erase(uint32_t addr)
{
    HAL_StatusTypeDef st;

    flash_cs_low();
    flash_send_cmd(0x06);                    // 写使能
    flash_cs_high();

    flash_cs_low();
    flash_send_cmd(0x20);                    // 扇区擦除
    flash_send_addr(addr);
    st = HAL_SPI_Transmit(&hspi2, NULL, 0, 100);  // 命令已发完
    flash_cs_high();

    if (st != HAL_OK) return st;
    return spi_flash_w25q128_wait_ready(1000);   // 扇区擦除约几十~几百 ms
}

HAL_StatusTypeDef spi_flash_w25q128_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0)
        return HAL_ERROR;

    uint32_t written = 0;
    while (written < len) {
        // 计算当前页剩余空间（每页 256B，不能跨页写）
        uint32_t page_offset = (addr + written) % W25Q128_PAGE_SIZE_BYTES;
        uint32_t chunk = W25Q128_PAGE_SIZE_BYTES - page_offset;
        if (chunk > (len - written)) chunk = len - written;

        // 写使能
        flash_cs_low();
        flash_send_cmd(0x06);
        flash_cs_high();

        // 页编程
        flash_cs_low();
        flash_send_cmd(0x02);
        flash_send_addr(addr + written);
        HAL_SPI_Transmit(&hspi2, (uint8_t *)buf + written, chunk, 1000);
        flash_cs_high();

        HAL_StatusTypeDef st = spi_flash_w25q128_wait_ready(100);
        if (st != HAL_OK) return st;

        written += chunk;
    }
    return HAL_OK;
}
