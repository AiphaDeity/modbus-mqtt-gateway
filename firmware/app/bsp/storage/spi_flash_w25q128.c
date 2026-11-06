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

/* ---------- SPI2 总线互斥锁 ---------- */

osMutexId_t g_m3_spi2_mutex = NULL;

static const osMutexAttr_t spi2_mutex_attr = {
    .name      = "m3_spi2",
    .attr_bits = osMutexRecursive | osMutexPrioInherit,
    .cb_mem    = NULL,
    .cb_size   = 0,
};

// 锁创建前（如初始化早期）允许直接访问，避免空指针
static void flash_lock(void)
{
    if (g_m3_spi2_mutex != NULL) {
        (void)osMutexAcquire(g_m3_spi2_mutex, osWaitForever);
    }
}

static void flash_unlock(void)
{
    if (g_m3_spi2_mutex != NULL) {
        (void)osMutexRelease(g_m3_spi2_mutex);
    }
}

/* ---------- 内部工具 ---------- */

static void flash_cs_low(void)  { HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_RESET); }
static void flash_cs_high(void) { HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_SET); }

/*
 * W25Q128 硬件复位（正确序列）：
 *   1. Reset Enable  (0x66) — 必须先发此命令才能接受 Reset
 *   2. Reset         (0x99) — 触发芯片复位
 * 复位后芯片需要 tPOR（最大 50ms）才能响应命令，
 * 调用方在 reset 返回后负责延时。
 *
 * 注意：本函数内部不加 SPI2 锁，由调用方负责。
 */
static void flash_hw_reset(void)
{
    uint8_t cmd = 0x66;   // Reset Enable
    flash_cs_low();
    (void)HAL_SPI_Transmit(&hspi2, &cmd, 1, 100);
    flash_cs_high();

    cmd = 0x99;           // Reset
    flash_cs_low();
    (void)HAL_SPI_Transmit(&hspi2, &cmd, 1, 100);
    flash_cs_high();
}

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

    flash_lock();
    do {
        flash_cs_low();
        flash_send_cmd(0x05);                 // 读状态寄存器
        HAL_SPI_Receive(&hspi2, &status, 1, 100);
        flash_cs_high();
        if ((status & 0x01) == 0) {           // bit0 = 0 表示空闲
            flash_unlock();
            return HAL_OK;
        }
    } while ((HAL_GetTick() - start) < timeout_ms);

    flash_unlock();
    return HAL_TIMEOUT;
}

/* ---------- 对外 API ---------- */

HAL_StatusTypeDef spi_flash_w25q128_init(void)
{
    // 首次初始化时创建 SPI2 总线锁
    if (g_m3_spi2_mutex == NULL) {
        g_m3_spi2_mutex = osMutexNew(&spi2_mutex_attr);
        if (g_m3_spi2_mutex == NULL) return HAL_ERROR;
    }

    /*
     * W25Q128 上电后需要 tPOR（最大 50ms）内部启动。
     * 在 Storage_Init 阶段 MCU 刚启动几毫秒，Flash 还没 ready。
     * 策略：先等 20ms 让 Flash 至少完成部分上电，
     *       再发正确的硬件复位序列，每次复位后等 60ms，
     *       最多重试 5 次。
     */
    HAL_Delay(20);   // 首次上电等待

    const int max_retry = 5;
    for (int attempt = 0; attempt < max_retry; attempt++) {
        flash_lock();
        flash_hw_reset();
        flash_unlock();

        /* 复位后等待 tPOR + 余量（W25Q128 最大 50ms），多留余量确保稳定 */
        HAL_Delay(200);

        uint32_t id = spi_flash_w25q128_read_id();
        // JEDEC ID: (Manufacturer<<16) | (Type<<8) | Capacity
        // W25Q128: Manufacturer=0xEF, 只检查高字节即可
        if (((id >> 16) & 0xFF) == 0xEF)
            return HAL_OK;
    }
    return HAL_ERROR;
}

uint32_t spi_flash_w25q128_read_id(void)
{
    uint8_t buf[3] = {0, 0, 0};

    flash_lock();
    flash_cs_low();
    flash_send_cmd(0x9F);                    // JEDEC ID 命令
    HAL_SPI_Receive(&hspi2, buf, 3, 100);   // 读 3 字节
    flash_cs_high();
    flash_unlock();

    return ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
}

HAL_StatusTypeDef spi_flash_w25q128_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0)
        return HAL_ERROR;

    HAL_StatusTypeDef st;

    flash_lock();
    flash_cs_low();
    flash_send_cmd(0x03);                    // 读数据命令
    flash_send_addr(addr);
    st = HAL_SPI_Receive(&hspi2, buf, len, 1000);
    flash_cs_high();
    flash_unlock();

    return st;
}

HAL_StatusTypeDef spi_flash_w25q128_sector_erase(uint32_t addr)
{
    HAL_StatusTypeDef st;

    flash_lock();

    flash_cs_low();
    st = flash_send_cmd(0x06);               // 写使能
    flash_cs_high();
    if (st != HAL_OK) {
        flash_unlock();
        return st;
    }

    /*
     * 命令 + 地址必须看返回值。HAL_SPI_Transmit 是阻塞式，
     * 返回即代表发送完成，不要再补一次零长度传输——HAL 对 Size==0
     * 直接返回 HAL_ERROR，会让整个擦除路径永远报失败。
     */
    flash_cs_low();
    st = flash_send_cmd(0x20);               // 扇区擦除
    if (st == HAL_OK)
        st = flash_send_addr(addr);
    flash_cs_high();

    if (st != HAL_OK) {
        flash_unlock();
        return st;
    }

    st = spi_flash_w25q128_wait_ready(1000);  // 扇区擦除约几十~几百 ms
    flash_unlock();
    return st;
}

HAL_StatusTypeDef spi_flash_w25q128_block64_erase(uint32_t addr)
{
    HAL_StatusTypeDef st;

    flash_lock();

    flash_cs_low();
    st = flash_send_cmd(0x06);               // 写使能
    flash_cs_high();
    if (st != HAL_OK) {
        flash_unlock();
        return st;
    }

    flash_cs_low();
    st = flash_send_cmd(0xD8);               // 64KB 块擦除
    if (st == HAL_OK)
        st = flash_send_addr(addr);
    flash_cs_high();

    if (st != HAL_OK) {
        flash_unlock();
        return st;
    }

    st = spi_flash_w25q128_wait_ready(2000);  // 块擦除约 150~500 ms
    flash_unlock();
    return st;
}

HAL_StatusTypeDef spi_flash_w25q128_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0)
        return HAL_ERROR;

    uint32_t written = 0;

    flash_lock();
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
        if (st != HAL_OK) {
            flash_unlock();
            return st;
        }

        written += chunk;
    }
    flash_unlock();

    return HAL_OK;
}
