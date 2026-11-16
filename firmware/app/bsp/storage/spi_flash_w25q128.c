/*
 * W25Q64 SPI Flash 驱动实现。
 *
 * 硬件：SPI3，CS = PD11；PE4 保持高，让底板原 Flash 未选中。
 * 依赖：CubeMX 已生成 MX_SPI3_Init()，全局句柄 hspi3
 *       main.h 中已定义 EXT_SPI_FLASH_CS_Pin / EXT_SPI_FLASH_CS_GPIO_Port
 *
 * W25Q64 命令集：
 *   写使能        0x06
 *   读状态寄存器  0x05   （bit0 = BUSY，1=忙碌）
 *   读 JEDEC ID   0x9F   （返回 3 字节，W25Q64 = 0xEF4017）
 *   读数据        0x03   （+ 3 字节地址）
 *   页编程        0x02   （+ 3 字节地址 + 数据，每页最多 256B）
 *   扇区擦除 4KB  0x20   （+ 3 字节地址）
 */

#include "spi_flash_w25q128.h"
#include "stm32f4xx_hal_spi.h"
#include "main.h"   // 取 EXT_SPI_FLASH_CS_Pin / EXT_SPI_FLASH_CS_GPIO_Port
#include <string.h>

extern SPI_HandleTypeDef hspi3;

/* ---------- SPI3 总线互斥锁 ---------- */

osMutexId_t g_m3_spi3_mutex = NULL;

static const osMutexAttr_t spi3_mutex_attr = {
    .name      = "m3_spi3",
    .attr_bits = osMutexRecursive | osMutexPrioInherit,
    .cb_mem    = NULL,
    .cb_size   = 0,
};

// 初始化和诊断读取共用锁；取锁失败或外设仍忙时不访问SPI。
static bool flash_lock(void)
{
    if (g_m3_spi3_mutex != NULL &&
        osMutexAcquire(g_m3_spi3_mutex, osWaitForever) != osOK) {
        return false;
    }
    if (hspi3.State != HAL_SPI_STATE_READY) {
        if (g_m3_spi3_mutex != NULL) (void)osMutexRelease(g_m3_spi3_mutex);
        return false;
    }
    return true;
}

// 释放当前调用持有的SPI锁。
static void flash_unlock(void)
{
    if (g_m3_spi3_mutex != NULL) {
        (void)osMutexRelease(g_m3_spi3_mutex);
    }
}

/* ---------- 内部工具 ---------- */

/* HAL主机轮询收发已等待BSY；失败收尾仍限时检查，再释放片选。 */
static void spi_wait_idle(void)
{
    uint32_t start = HAL_GetTick();
    while ((hspi3.Instance->SR & SPI_SR_BSY) != 0) {
        if ((HAL_GetTick() - start) > 10) break;   // 10ms 安全超时
    }
}

// 选中Flash，开始一次命令事务。
static void flash_cs_low(void)  { HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_RESET); }

/* 连续命令之间保留片选高电平间隔。 */
static inline void flash_cs_deadtime(void)
{
    for (uint32_t i = 0; i < 32U; i++) {
        __NOP();
    }
}

// 收发结束或失败后释放片选，并保留命令间隔。
static void flash_cs_high(void)
{
    spi_wait_idle();
    HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_SET);
    flash_cs_deadtime();
}

// 复位使能失败时不继续发送复位；调用方持锁并负责后续上电等待。
static HAL_StatusTypeDef flash_hw_reset(void)
{
    uint8_t cmd = 0x66;
    flash_cs_low();
    HAL_StatusTypeDef st = HAL_SPI_Transmit(&hspi3, &cmd, 1, 100);
    flash_cs_high();
    if (st != HAL_OK) return st;

    cmd = 0x99;
    flash_cs_low();
    st = HAL_SPI_Transmit(&hspi3, &cmd, 1, 100);
    flash_cs_high();
    return st;
}

/* ---------- 对外 API ---------- */

// 创建总线锁并复位芯片，确认厂家和容量匹配固定分区。
HAL_StatusTypeDef spi_flash_w25q128_init(void)
{
    // 首次初始化时创建 SPI3 总线锁
    if (g_m3_spi3_mutex == NULL) {
        g_m3_spi3_mutex = osMutexNew(&spi3_mutex_attr);
        if (g_m3_spi3_mutex == NULL) return HAL_ERROR;
    }

    HAL_Delay(20);   // 首次上电等待

    const int max_retry = 5;
    for (int attempt = 0; attempt < max_retry; attempt++) {
        if (!flash_lock()) return HAL_BUSY;
        HAL_StatusTypeDef st = flash_hw_reset();
        flash_unlock();
        if (st != HAL_OK) return st;

        // 等待芯片退出复位后再读取ID。
        HAL_Delay(200);

        uint32_t id = spi_flash_w25q128_read_id();
        // 厂家、类型和容量均须匹配，避免错误芯片通过初始化。
        if (id == FLASH_JEDEC_ID)
            return HAL_OK;
    }
    return HAL_ERROR;
}

// 一次片选内读取完整JEDEC ID，传输失败不使用部分接收数据。
uint32_t spi_flash_w25q128_read_id(void)
{
    uint8_t tx[4] = { 0x9F, 0xFF, 0xFF, 0xFF };   // 命令 + 3 字节空拍
    uint8_t rx[4] = { 0, 0, 0, 0 };

    if (!flash_lock()) return 0xFFFFFFU;
    flash_cs_low();
    HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&hspi3, tx, rx, 4, 100);
    flash_cs_high();
    flash_unlock();
    if (st != HAL_OK) return 0xFFFFFFU;

    /* rx[0] 是发送 0x9F 期间同步收到的空字节，真正的 ID 在 rx[1..3] */
    return ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | rx[3];
}

// 状态读取失败返回FF，不能把未接收的零值作为空闲状态。
static uint8_t flash_read_sr(uint8_t cmd)
{
    uint8_t tx[2] = { cmd, 0xFF };
    uint8_t rx[2] = { 0, 0 };

    if (!flash_lock()) return 0xFFU;
    flash_cs_low();
    HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&hspi3, tx, rx, 2, 100);
    flash_cs_high();
    flash_unlock();

    return st == HAL_OK ? rx[1] : 0xFFU;
}

// 读取状态寄存器1，供存储故障日志使用。
uint8_t spi_flash_w25q128_read_sr1(void) { return flash_read_sr(0x05); }
// 读取状态寄存器2，供存储故障日志使用。
uint8_t spi_flash_w25q128_read_sr2(void) { return flash_read_sr(0x35); }

/* ---------- 有界存储I/O ---------- */

/* 绝对期限的间隔必须小于INT32_MAX ms，支持HAL tick回绕。 */
static uint32_t flash_remaining(uint32_t deadline_ms)
{
    int32_t remaining = (int32_t)(deadline_ms - HAL_GetTick());
    return remaining > 0 ? (uint32_t)remaining : 0U;
}

/* 使用剩余预算取SPI锁，不沿用普通接口的无限等待。 */
static HAL_StatusTypeDef flash_lock_until(uint32_t deadline_ms)
{
    uint32_t remaining = flash_remaining(deadline_ms);
    if (remaining == 0U) return HAL_TIMEOUT;
    if (g_m3_spi3_mutex == NULL) return HAL_ERROR;

    uint64_t ticks = ((uint64_t)remaining * osKernelGetTickFreq() + 999U) / 1000U;
    osStatus_t st = osMutexAcquire(g_m3_spi3_mutex, (uint32_t)ticks);
    if (st != osOK) return st == osErrorTimeout ? HAL_TIMEOUT : HAL_ERROR;
    if (flash_remaining(deadline_ms) == 0U || hspi3.State != HAL_SPI_STATE_READY) {
        flash_unlock();
        return flash_remaining(deadline_ms) == 0U ? HAL_TIMEOUT : HAL_BUSY;
    }
    return HAL_OK;
}

/* 一次CS窗口内完成命令与数据，失败仍释放片选。调用方已持锁。 */
static HAL_StatusTypeDef flash_transfer_until(uint8_t *tx, uint8_t *rx,
                                              uint16_t len, uint32_t deadline_ms)
{
    uint32_t remaining = flash_remaining(deadline_ms);
    if (remaining == 0U) return HAL_TIMEOUT;
    flash_cs_low();
    HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&hspi3, tx, rx, len, remaining);
    while ((hspi3.Instance->SR & SPI_SR_BSY) != 0U) {
        if (flash_remaining(deadline_ms) == 0U) {
            st = HAL_TIMEOUT;
            break;
        }
    }
    HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_SET);
    flash_cs_deadtime();
    return flash_remaining(deadline_ms) == 0U ? HAL_TIMEOUT : st;
}

/* 读取SR1；传输失败不得用未更新的状态字节判断空闲。 */
static HAL_StatusTypeDef flash_status_until(uint8_t *status, uint32_t deadline_ms)
{
    uint8_t tx[2] = {0x05, 0xFF};
    uint8_t rx[2] = {0, 0};
    HAL_StatusTypeDef st = flash_transfer_until(tx, rx, 2U, deadline_ms);
    if (st == HAL_OK) *status = rx[1];
    return st;
}

/* 等待两次连续空闲，避免命令刚发出时BUSY尚未置位；不延长原期限。 */
static HAL_StatusTypeDef flash_ready_until(uint32_t deadline_ms)
{
    uint32_t ready = 0U;
    do {
        uint8_t status;
        HAL_StatusTypeDef st = flash_status_until(&status, deadline_ms);
        if (st != HAL_OK) return st;
        ready = (status & 1U) == 0U ? ready + 1U : 0U;
        if (ready == 2U) return HAL_OK;
        osDelay(1U);
    } while (flash_remaining(deadline_ms) != 0U);
    return HAL_TIMEOUT;
}

/* 写使能后核对WEL；读失败或保护状态异常时不发修改命令。 */
static HAL_StatusTypeDef flash_enable_until(uint32_t deadline_ms)
{
    uint8_t tx = 0x06;
    uint8_t rx;
    uint8_t status;
    HAL_StatusTypeDef st = flash_ready_until(deadline_ms);
    if (st == HAL_OK) st = flash_transfer_until(&tx, &rx, 1U, deadline_ms);
    if (st == HAL_OK) st = flash_status_until(&status, deadline_ms);
    if (st == HAL_OK && (status & 2U) == 0U) st = HAL_ERROR;
    return st;
}

/* 读取一块升级数据；地址、长度及所有等待都受同一期限限制。 */
HAL_StatusTypeDef spi_flash_w25q128_read_until(uint32_t addr, uint8_t *buf,
                                             uint32_t len, uint32_t deadline_ms)
{
    if (buf == NULL || len == 0U || len > W25Q128_PAGE_SIZE_BYTES ||
        addr >= W25Q128_FLASH_SIZE_BYTES || len > W25Q128_FLASH_SIZE_BYTES - addr)
        return HAL_ERROR;

    HAL_StatusTypeDef st = flash_lock_until(deadline_ms);
    if (st != HAL_OK) return st;
    uint8_t tx[4 + W25Q128_PAGE_SIZE_BYTES];
    uint8_t rx[4 + W25Q128_PAGE_SIZE_BYTES];
    tx[0] = 0x03;
    tx[1] = (uint8_t)(addr >> 16);
    tx[2] = (uint8_t)(addr >> 8);
    tx[3] = (uint8_t)addr;
    memset(tx + 4, 0xFF, len);
    uint8_t status;
    st = flash_status_until(&status, deadline_ms);
    if (st == HAL_OK && (status & 1U) != 0U) st = flash_ready_until(deadline_ms);
    if (st == HAL_OK) st = flash_transfer_until(tx, rx, (uint16_t)(4U + len), deadline_ms);
    if (st == HAL_OK) memcpy(buf, rx + 4, len);
    flash_unlock();
    return st;
}

/* 分页编程；每页开始前复查期限，原命令超时后不继续下一页。 */
HAL_StatusTypeDef spi_flash_w25q128_write_until(uint32_t addr, const uint8_t *buf,
                                              uint32_t len, uint32_t deadline_ms)
{
    if (buf == NULL || len == 0U || len > W25Q128_PAGE_SIZE_BYTES ||
        addr >= W25Q128_FLASH_SIZE_BYTES || len > W25Q128_FLASH_SIZE_BYTES - addr)
        return HAL_ERROR;

    HAL_StatusTypeDef st = flash_lock_until(deadline_ms);
    if (st != HAL_OK) return st;
    uint8_t tx[4 + W25Q128_PAGE_SIZE_BYTES];
    uint8_t rx[4 + W25Q128_PAGE_SIZE_BYTES];
    uint32_t done = 0U;
    while (done < len) {
        uint32_t address = addr + done;
        uint32_t chunk = W25Q128_PAGE_SIZE_BYTES - address % W25Q128_PAGE_SIZE_BYTES;
        if (chunk > len - done) chunk = len - done;
        tx[0] = 0x02;
        tx[1] = (uint8_t)(address >> 16);
        tx[2] = (uint8_t)(address >> 8);
        tx[3] = (uint8_t)address;
        memcpy(tx + 4, buf + done, chunk);
        st = flash_enable_until(deadline_ms);
        if (st == HAL_OK) st = flash_transfer_until(tx, rx, (uint16_t)(4U + chunk), deadline_ms);
        if (st == HAL_OK) st = flash_ready_until(deadline_ms);
        if (st != HAL_OK) break;
        done += chunk;
    }
    flash_unlock();
    return st;
}

/* 擦除一个对齐扇区或块；不把超时解释为擦除已取消。 */
HAL_StatusTypeDef spi_flash_w25q128_erase_until(uint32_t addr, bool block64,
                                              uint32_t deadline_ms)
{
    uint32_t size = block64 ? 65536U : W25Q128_SECTOR_SIZE_BYTES;
    if (addr >= W25Q128_FLASH_SIZE_BYTES || addr % size != 0U ||
        size > W25Q128_FLASH_SIZE_BYTES - addr) return HAL_ERROR;

    HAL_StatusTypeDef st = flash_lock_until(deadline_ms);
    if (st != HAL_OK) return st;
    uint8_t tx[4] = {block64 ? 0xD8 : 0x20,
                    (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    uint8_t rx[4];
    st = flash_enable_until(deadline_ms);
    if (st == HAL_OK) st = flash_transfer_until(tx, rx, 4U, deadline_ms);
    if (st == HAL_OK) st = flash_ready_until(deadline_ms);
    flash_unlock();
    return st;
}
