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
#include <stdio.h>

extern SPI_HandleTypeDef hspi2;

/* ---------- SPI2 总线互斥锁 ---------- */

osMutexId_t g_m3_spi2_mutex = NULL;

/*
 * 底层驱动过程打印开关 M3_FLASH_TRACE（默认 0）。
 *
 * 排查时序问题时改成 1，会打印状态轮询的每一轮返回值
 * （耗时、轮次、两个返回字节、BSY 位、SPI 状态机状态）。
 * 正常运行时关闭 —— 一次擦除要轮询几十轮，开着会刷屏。
 */
#ifndef M3_FLASH_TRACE
#define M3_FLASH_TRACE 0
#endif

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
    /* 防御性：如果 SPI 状态机卡死（非 READY），先 abort 让它回到空闲态，
     * 否则后续所有 SPI 操作都会静默失败且难以排查。
     */
    if (hspi2.State != HAL_SPI_STATE_READY) {
        (void)HAL_SPI_Abort(&hspi2);
    }
}

static void flash_unlock(void)
{
    if (g_m3_spi2_mutex != NULL) {
        (void)osMutexRelease(g_m3_spi2_mutex);
    }
}

/* ---------- 内部工具 ---------- */

/*
 * 拉高 CS 前必须等待 SPI 自身 BSY 标志清零。
 * HAL_SPI_Transmit / TransmitReceive 在 polling 模式下只等 TXE
 * （TX buffer empty），不等 BSY（shift register empty）。如果
 * CS 在 shift register 里的最后一字节还没发完时被拉高，
 * W25Q128 会截断这一字节——表现为"写入前 N 字节正确、
 * 最后几字节错乱/FF"，读操作同理。
 */
static void spi_wait_idle(void)
{
    uint32_t start = HAL_GetTick();
    while ((hspi2.Instance->SR & SPI_SR_BSY) != 0) {
        if ((HAL_GetTick() - start) > 10) break;   // 10ms 安全超时
    }
}

static void flash_cs_low(void)  { HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_RESET); }

/*
 * CS 高电平最小保持时间（tCSW）。
 *
 * W25Q128 要求 CS 保持高电平至少 50ns 才算"命令结束"。HAL_SPI_Transmit /
 * TransmitReceive 返回后内部已经关了 SPI，拉高 CS 到下次拉低之间只剩
 * 几条 CPU 指令（约 10ns），理论上不够。所以拉高后主动补一段死区。
 *
 * 为什么不用 HAL_Delay(1)：1ms 是 50ns 的 20000 倍。当前 SPI2 跑 164kHz
 * 时看不出来，等分频降到 5.25MHz（256 字节只要 0.4ms）之后，每次事务
 * 白等 1ms 就会比传输本身还长，成为明显瓶颈。
 *
 * 168MHz 下 1 个 NOP ≈ 6ns，32 个约 200ns，是 tCSW 的 4 倍裕量。
 * 用显式 __NOP() 而不是空循环，避免被编译器优化掉。
 */
static inline void flash_cs_deadtime(void)
{
    for (uint32_t i = 0; i < 32U; i++) {
        __NOP();
    }
}

static void flash_cs_high(void)
{
    spi_wait_idle();
    HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_SET);
    flash_cs_deadtime();
}

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

// 等待写入/擦除完成（轮询状态寄存器 bit0）
HAL_StatusTypeDef spi_flash_w25q128_wait_ready(uint32_t timeout_ms)
{
    /*
     * 三个要点，缺一个都会误判：
     *   ① 命令(0x05) 与状态字节放在同一次 CS 窗口内收发（TransmitReceive），
     *      避免"分两次 HAL 调用"造成的首字节错位；
     *   ② 命令刚结束时芯片内部周期可能还没置起 BUSY，第一次读状态会得到
     *      0x00，若据此判"空闲"，擦除/编程根本没等就返回了；
     *   ③ 所以先给 1ms 让状态稳定，再要求连续两次读到 BUSY=0 才算完成。
     */
    uint8_t tx[2] = { 0x05, 0xFF };
    uint8_t rx[2];
    uint32_t start = HAL_GetTick();
    uint32_t ready_cnt = 0;
#if M3_FLASH_TRACE
    int loop = 0;
#endif

    HAL_Delay(1);

    flash_lock();
    do {
        rx[1] = 0xFF;
        flash_cs_low();
        (void)HAL_SPI_TransmitReceive(&hspi2, tx, rx, 2, 100);
        flash_cs_high();

#if M3_FLASH_TRACE
        loop++;
        printf("[wait_ready] t=%lu loop=%d rx[0]=0x%02X rx[1]=0x%02X BSY=%d rx_cnt=%lu HAL_state=%d\r\n",
               (unsigned long)(HAL_GetTick()-start), loop, rx[0], rx[1],
               (int)(rx[1] & 0x01), (unsigned long)ready_cnt, (int)hspi2.State);
#endif

        if ((rx[1] & 0x01) == 0) {            // bit0 = 0 表示空闲
            if (++ready_cnt >= 2) {
                flash_unlock();
                return HAL_OK;
            }
        } else {
            ready_cnt = 0;
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
    /*
     * 命令(0x9F) 与 3 字节 ID 放在同一次 CS 窗口内收发（TransmitReceive），
     * 与 flash_read_sr / wait_ready 保持同一套读法。
     * 拆成 Transmit + Receive 两次 HAL 调用时，F4 HAL 会在 Transmit 结束时
     * __HAL_SPI_DISABLE()，CS 仍为低，SCK 的状态变化被 Flash 多计一个时钟，
     * 读回的 ID 会整体错位（历史上出现过 BA0180 / 000000 两种症状）。
     */
    uint8_t tx[4] = { 0x9F, 0xFF, 0xFF, 0xFF };   // 命令 + 3 字节空拍
    uint8_t rx[4] = { 0, 0, 0, 0 };

    flash_lock();
    flash_cs_low();
    (void)HAL_SPI_TransmitReceive(&hspi2, tx, rx, 4, 100);
    flash_cs_high();
    flash_unlock();

    /* rx[0] 是发送 0x9F 期间同步收到的空字节，真正的 ID 在 rx[1..3] */
    return ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | rx[3];
}

/*
 * 【临时诊断】读状态寄存器。
 * 命令与接收放在同一次 TransmitReceive 里，避免"分两次 HAL 调用"
 * 可能带来的首字节错位，保证诊断值本身可信。
 */
static uint8_t flash_read_sr(uint8_t cmd)
{
    uint8_t tx[2] = { cmd, 0xFF };
    uint8_t rx[2] = { 0, 0 };

    flash_lock();
    flash_cs_low();
    (void)HAL_SPI_TransmitReceive(&hspi2, tx, rx, 2, 100);
    flash_cs_high();
    flash_unlock();

    return rx[1];
}

uint8_t spi_flash_w25q128_read_sr1(void) { return flash_read_sr(0x05); }
uint8_t spi_flash_w25q128_read_sr2(void) { return flash_read_sr(0x35); }

HAL_StatusTypeDef spi_flash_w25q128_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0)
        return HAL_ERROR;

    /*
     * 整个事务（命令 + 地址 + 数据）必须在同一个 CS 窗口里用
     * 一次 TransmitReceive 走完。F4 HAL 每次独立的 Transmit/Receive
     * 返回前都会内部 __HAL_SPI_DISABLE()，CS 还低时 SPI 关闭瞬间
     * SCK 回落到空闲电平被 Flash 当成额外时钟——后续命令/地址/数据
     * 整体错位，这就是"写 A 后假成功、回读全乱"的根因。
     *
     * 数据按 256B 分块循环，避免一次性拼几千字节的栈缓冲。
     */
    HAL_StatusTypeDef st = HAL_OK;
    uint32_t offset = 0;

    flash_lock();
    while (offset < len) {
        uint32_t chunk = len - offset;
        if (chunk > W25Q128_PAGE_SIZE_BYTES) chunk = W25Q128_PAGE_SIZE_BYTES;

        uint8_t tx[4 + W25Q128_PAGE_SIZE_BYTES];
        uint8_t rx[4 + W25Q128_PAGE_SIZE_BYTES];

        tx[0] = 0x03;                              // 读数据命令
        tx[1] = (uint8_t)((addr + offset) >> 16);
        tx[2] = (uint8_t)((addr + offset) >> 8);
        tx[3] = (uint8_t)( addr + offset);
        /* 空拍 0xFF 产生时钟，驱动 MISO 返回数据 */
        for (int i = 0; i < (int)chunk; i++) tx[4 + i] = 0xFF;

        flash_cs_low();
        st = HAL_SPI_TransmitReceive(&hspi2, tx, rx, 4 + chunk, 1000);
        flash_cs_high();

        if (st != HAL_OK) break;

        /* rx[0..3] 是发命令/地址期间同步收到的空字节，真正数据从 rx[4] 开始 */
        for (int i = 0; i < (int)chunk; i++) buf[offset + i] = rx[4 + i];

        offset += chunk;
    }
    flash_unlock();

    return st;
}

HAL_StatusTypeDef spi_flash_w25q128_sector_erase(uint32_t addr)
{
    HAL_StatusTypeDef st;

    flash_lock();

    // 写使能（独立短事务，CS 单独拉一次，没问题）
    flash_cs_low();
    (void)HAL_SPI_Transmit(&hspi2, (uint8_t[]){0x06}, 1, 100);
    flash_cs_high();

    /*
     * 扇区擦除：命令 0x20 + 3 字节地址，一次 TransmitReceive 走完。
     * 原来分 flash_send_cmd(0x20) + flash_send_addr() 两次 HAL 调用，
     * 每次返回前内部 __HAL_SPI_DISABLE()，CS 还低时地址字节会整体错位。
     */
    {
        uint8_t tx[4] = {
            0x20,
            (uint8_t)((addr >> 16) & 0xFF),
            (uint8_t)((addr >> 8)  & 0xFF),
            (uint8_t)( addr        & 0xFF),
        };
        uint8_t rx[4];

        flash_cs_low();
        st = HAL_SPI_TransmitReceive(&hspi2, tx, rx, 4, 100);
        flash_cs_high();
    }

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

    // 写使能（独立短事务，CS 单独拉一次，没问题）
    flash_cs_low();
    (void)HAL_SPI_Transmit(&hspi2, (uint8_t[]){0x06}, 1, 100);
    flash_cs_high();

    /*
     * 64KB 块擦除：命令 0xD8 + 3 字节地址，一次 TransmitReceive 走完。
     * 同 sector_erase，原来分两次 HAL 调用会导致地址错位。
     */
    {
        uint8_t tx[4] = {
            0xD8,
            (uint8_t)((addr >> 16) & 0xFF),
            (uint8_t)((addr >> 8)  & 0xFF),
            (uint8_t)( addr        & 0xFF),
        };
        uint8_t rx[4];

        flash_cs_low();
        st = HAL_SPI_TransmitReceive(&hspi2, tx, rx, 4, 100);
        flash_cs_high();
    }

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

    /*
     * 每一页的编程事务（页编程命令 + 3 字节地址 + 页内数据）
     * 必须在同一个 CS 窗口里用一次 TransmitReceive 走完。
     * 写使能 0x06 是独立短事务（1 字节命令，CS 单独拉一次），
     * 保持原样——单字节不会触发错位。
     */
    uint32_t written = 0;
    HAL_StatusTypeDef st = HAL_OK;

    flash_lock();
    while (written < len) {
        // 计算当前页剩余空间（每页 256B，不能跨页写）
        uint32_t page_offset = (addr + written) % W25Q128_PAGE_SIZE_BYTES;
        uint32_t chunk = W25Q128_PAGE_SIZE_BYTES - page_offset;
        if (chunk > (len - written)) chunk = len - written;

        // 写使能（独立短事务，CS 单独拉一次，没问题）
        flash_cs_low();
        (void)HAL_SPI_Transmit(&hspi2, (uint8_t[]){0x06}, 1, 100);
        flash_cs_high();

        // 页编程：cmd + 3addr + chunk 数据 一次走完
        uint8_t tx[4 + W25Q128_PAGE_SIZE_BYTES];
        uint8_t rx[4 + W25Q128_PAGE_SIZE_BYTES];

        tx[0] = 0x02;
        tx[1] = (uint8_t)((addr + written) >> 16);
        tx[2] = (uint8_t)((addr + written) >> 8);
        tx[3] = (uint8_t)( addr + written);
        for (int i = 0; i < (int)chunk; i++) tx[4 + i] = buf[written + i];

        flash_cs_low();
        st = HAL_SPI_TransmitReceive(&hspi2, tx, rx, 4 + chunk, 1000);
        flash_cs_high();

        if (st != HAL_OK) {
            flash_unlock();
            return st;
        }

        st = spi_flash_w25q128_wait_ready(100);
        if (st != HAL_OK) {
            flash_unlock();
            return st;
        }

        written += chunk;
    }
    flash_unlock();

    return HAL_OK;
}
