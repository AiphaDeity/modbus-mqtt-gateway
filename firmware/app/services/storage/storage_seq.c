/*
 * storage_seq.c — 序列号管理（双份冗余高水位）
 *
 * 持久化位置：身份区 0x042000 / 0x043000（各占一个 4KiB 扇区）。
 * 每份布局：[magic(4B)][next_reserved(8B)][crc32(4B)]
 *
 * 写入时覆盖"较旧的那份"，保留较新的一份作掉电回退。
 * 启动时取两份中有效且较大的高水位，避免序号复用。
 */

#include "storage_seq.h"
#include "flash_partition.h"
#include "spi_flash_w25q128.h"
#include "cmsis_os2.h"
#include <string.h>
#include <stdio.h>

#define SEQ_MAGIC   0x53455131U  /* "SEQ1" */

/*
 * 过程打印开关 M3_SEQ_TRACE（默认 0）。
 *
 * 排查"写入假成功"时改成 1，会打印擦除/写入每一步的返回值、编程后的
 * 状态寄存器、以及回读的期望值与实际字节。这些信息定位问题时很好用，
 * 但正常运行时每条都占串口时间，所以默认关闭。
 *
 * 常驻保留的只有结果行和异常行：
 *   init read A / init read B          开机各一行
 *   alloc overflow / copy_write failed / verify FAIL   出问题才打
 */
#ifndef M3_SEQ_TRACE
#define M3_SEQ_TRACE 0
#endif

#if M3_SEQ_TRACE
#define SEQ_TRACE(...)   do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#else
#define SEQ_TRACE(...)   ((void)0)
#endif


typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint64_t next_reserved;
    uint32_t crc32;
} seq_meta_t;

_Static_assert(sizeof(seq_meta_t) == 16, "seq_meta_t must be 16 bytes");

#define SEQ_CRC_LEN (sizeof(seq_meta_t) - sizeof(uint32_t))

static const uint32_t s_copy_addr[2] = { FLASH_SEQ_A_ADDR, FLASH_SEQ_B_ADDR };

static uint64_t s_next_reserved = 0;   /* 已持久化的高水位 */
static uint64_t s_current       = 0;   /* 当前可分配序号 */
static uint64_t s_window_end    = 0;   /* 当前窗口结束（不含） */
static bool     s_inited        = false;
static int      s_newer         = -1;  /* 高水位所在副本索引 */

/*
 * 序列号级互斥：底层 SPI 访问已有总线锁，但"窗口预留 → 擦写 → 回读"
 * 是一组复合操作，且 s_current++ 是读改写。多个任务并发 alloc 会拿到
 * 重复序号，或用旧高水位互相覆盖，因此这里再加一把独立的锁。
 */
static osMutexId_t s_seq_mutex = NULL;

static bool seq_mutex_init(void)
{
    if (s_seq_mutex != NULL) return true;

    const osMutexAttr_t attr = {
        .name      = "m3_seq",
        .attr_bits = osMutexPrioInherit,
        .cb_mem    = NULL,
        .cb_size   = 0,
    };
    s_seq_mutex = osMutexNew(&attr);
    return s_seq_mutex != NULL;
}

static void seq_lock(void)
{
    if (s_seq_mutex != NULL) (void)osMutexAcquire(s_seq_mutex, osWaitForever);
}

static void seq_unlock(void)
{
    if (s_seq_mutex != NULL) (void)osMutexRelease(s_seq_mutex);
}

static uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

static bool copy_read(int idx, seq_meta_t *meta)
{
    if (spi_flash_w25q128_read(s_copy_addr[idx], (uint8_t *)meta,
                               sizeof(*meta)) != HAL_OK) {
        return false;
    }
    if (meta->magic != SEQ_MAGIC) return false;
    if (meta->crc32 != crc32_calc((const uint8_t *)meta, SEQ_CRC_LEN)) return false;
    return true;
}

#if M3_SEQ_TRACE
/* 按字节打印元数据，便于对比"写进去的"与"读回来的" */
static void dump16(const char *tag, const uint8_t *p, uint32_t len)
{
    printf("[SEQ]   %s:", tag);
    for (uint32_t i = 0; i < len; i++) {
        printf(" %02X", (unsigned)p[i]);
    }
    printf("\r\n");
}
#endif /* M3_SEQ_TRACE */

static HAL_StatusTypeDef copy_write(int idx, uint64_t next_reserved)
{
    seq_meta_t meta;
    meta.magic         = SEQ_MAGIC;
    meta.next_reserved = next_reserved;
    meta.crc32         = crc32_calc((const uint8_t *)&meta, SEQ_CRC_LEN);

    SEQ_TRACE("[SEQ] copy_write idx=%d addr=0x%06lX new_resv=0x%08lX%08lX\r\n",
              idx, (unsigned long)s_copy_addr[idx],
              (unsigned long)(next_reserved >> 32), (unsigned long)(next_reserved));

    /* 擦除前后各打一条，用于判断复位是否发生在擦除过程中 */
    SEQ_TRACE("[SEQ] erase begin addr=0x%06lX\r\n", (unsigned long)s_copy_addr[idx]);

    HAL_StatusTypeDef st = spi_flash_w25q128_sector_erase(s_copy_addr[idx]);
    SEQ_TRACE("[SEQ] erase st=%d\r\n", (int)st);
    if (st != HAL_OK) return st;

    st = spi_flash_w25q128_write(s_copy_addr[idx], (const uint8_t *)&meta,
                                    sizeof(meta));
    SEQ_TRACE("[SEQ] write st=%d\r\n", (int)st);

#if M3_SEQ_TRACE
    /* 编程结束后看状态寄存器。
     *   WEL=1 → 写使能还挂着，说明 PROGRAM/ERASE 根本没被执行；
     *   WEL=0 → Flash 自己清掉了写使能，编程是真的发生了。
     * 这两者对应"写失败"和"写成功"，比看返回值更可靠。 */
    if (st == HAL_OK) {
        uint8_t sr1 = spi_flash_w25q128_read_sr1();
        uint8_t sr2 = spi_flash_w25q128_read_sr2();
        printf("[SEQ] post-write SR1=0x%02X BUSY=%d WEL=%d BP=%d%d%d SRP0=%d | "
               "SR2=0x%02X SRP1=%d\r\n",
               (unsigned)sr1, (int)(sr1 & 0x01), (int)((sr1 >> 1) & 0x01),
               (int)((sr1 >> 4) & 0x01), (int)((sr1 >> 3) & 0x01),
               (int)((sr1 >> 2) & 0x01), (int)((sr1 >> 7) & 0x01),
               (unsigned)sr2, (int)(sr2 & 0x01));
        fflush(stdout);
    }
#endif
    return st;
}

HAL_StatusTypeDef storage_seq_init(void)
{
    seq_meta_t meta_a, meta_b;
    bool ok_a, ok_b;

    if (!seq_mutex_init()) return HAL_ERROR;

    seq_lock();

    ok_a = copy_read(0, &meta_a);
    printf("[SEQ] init read A: ok=%d magic=0x%08lX crc=0x%08lX\r\n", ok_a,
           (unsigned long)meta_a.magic, (unsigned long)meta_a.crc32); fflush(stdout);
    ok_b = copy_read(1, &meta_b);
    printf("[SEQ] init read B: ok=%d magic=0x%08lX crc=0x%08lX\r\n", ok_b,
           (unsigned long)meta_b.magic, (unsigned long)meta_b.crc32); fflush(stdout);

    s_newer = -1;
    if (ok_a && ok_b) {
        s_newer = (meta_a.next_reserved >= meta_b.next_reserved) ? 0 : 1;
    } else if (ok_a) {
        s_newer = 0;
    } else if (ok_b) {
        s_newer = 1;
    }

    if (s_newer >= 0) {
        const seq_meta_t *meta = (s_newer == 0) ? &meta_a : &meta_b;
        s_next_reserved = meta->next_reserved;
    } else {
        /* 空设备：高水位从首发序号起（M2 约定全局 seq 从 1001 开始） */
        s_next_reserved = SEQ_FIRST_VALUE;
    }

    s_current    = s_next_reserved;
    s_window_end = s_next_reserved;

    /*
     * 空设备：把初始高水位落下，避免下一次写入时两份都为空。
     *
     * ⚠️ s_inited 必须在落盘成功之后才置位。原实现先把 s_inited 置 true，
     *    再 copy_write；一旦 copy_write 失败（擦除/写入错误），函数返回错误，
     *    但内存里已经是"已初始化"状态，后续 alloc 照常放号，而 Flash 里
     *    没有任何高水位 —— 掉电重启后两份都无效，seq 退回 SEQ_FIRST_VALUE，
     *    已发出去的序号被重新分配，破坏"全局单调递增"契约。
     */
    if (s_newer < 0) {
        HAL_StatusTypeDef st = copy_write(0, SEQ_FIRST_VALUE);
        if (st != HAL_OK) {
            s_inited = false;       /* 保持未初始化，alloc 直接拒绝 */
            seq_unlock();
            return st;
        }
        s_newer = 0;
    }

    s_inited = true;
    seq_unlock();
    return HAL_OK;
}

HAL_StatusTypeDef storage_seq_alloc(uint64_t *seq_out)
{
    HAL_StatusTypeDef st;

    if (seq_out == NULL || !s_inited) {
        printf("[SEQ] alloc early_fail seq_out=%p inited=%d\r\n", (void*)seq_out, s_inited);
        fflush(stdout);
        return HAL_ERROR;
    }

    seq_lock();

    SEQ_TRACE("[SEQ] alloc enter s_current=0x%08lX%08lX s_window_end=0x%08lX%08lX\r\n",
              (unsigned long)(s_current >> 32), (unsigned long)s_current,
              (unsigned long)(s_window_end >> 32), (unsigned long)s_window_end);

    /* 窗口用完，预留下一批 */
    if (s_current >= s_window_end) {
        uint64_t new_reserved = s_next_reserved + SEQ_RESERVE_BATCH;

        /* 接近最大值则停止生成，不回绕 */
        if (new_reserved < s_next_reserved ||
            new_reserved > UINT64_MAX - SEQ_RESERVE_BATCH) {
            printf("[SEQ] alloc overflow\r\n"); fflush(stdout);
            seq_unlock();
            return HAL_ERROR;
        }

        /* 先持久化新高水位：覆盖较旧副本 */
        int target = (s_newer < 0) ? 0 : (1 - s_newer);
        SEQ_TRACE("[SEQ] window_end reached, target=%d new_reserved=0x%08lX%08lX\r\n",
                  target, (unsigned long)(new_reserved >> 32), (unsigned long)new_reserved);
        st = copy_write(target, new_reserved);
        if (st != HAL_OK) {
            printf("[SEQ] copy_write failed st=%d\r\n", (int)st); fflush(stdout);
            seq_unlock();
            return st;
        }

        /* 回读校验，失败则不发放新窗口内的号 */
        seq_meta_t verify = { 0 };
        if (!copy_read(target, &verify)) {
            printf("[SEQ] copy_read verify FAIL (CRC/magic mismatch)\r\n");
            fflush(stdout);
#if M3_SEQ_TRACE
            /* 对比"应该写入的"与"实际读回来的"原始字节：
             *   expect   准备写进去的元数据
             *   got(1st) 走正常 read 路径读回来的结构体
             *   got(raw) 直接读原始字节，绕过结构体解析
             * 三者不一致时，能区分是"读路径错"还是"写进去的本身就错"。 */
            {
                seq_meta_t exp_meta;
                uint8_t raw[sizeof(seq_meta_t)] = { 0 };

                exp_meta.magic         = SEQ_MAGIC;
                exp_meta.next_reserved = new_reserved;
                exp_meta.crc32         = crc32_calc((const uint8_t *)&exp_meta, SEQ_CRC_LEN);
                (void)spi_flash_w25q128_read(s_copy_addr[target], raw, sizeof(raw));

                dump16("expect ", (const uint8_t *)&exp_meta, sizeof(exp_meta));
                dump16("got(1st)", (const uint8_t *)&verify, sizeof(verify));
                dump16("got(raw)", raw, sizeof(raw));
                fflush(stdout);
            }
#endif /* M3_SEQ_TRACE */
            seq_unlock();
            return HAL_ERROR;
        }
        if (verify.next_reserved != new_reserved) {
            printf("[SEQ] verify mismatch expect=0x%08lX%08lX got=0x%08lX%08lX\r\n",
                   (unsigned long)(new_reserved>>32),(unsigned long)new_reserved,
                   (unsigned long)(verify.next_reserved>>32),(unsigned long)verify.next_reserved);
            fflush(stdout);
            seq_unlock();
            return HAL_ERROR;
        }

        s_newer         = target;
        s_next_reserved = new_reserved;
        s_window_end    = new_reserved;
    }

    *seq_out = s_current++;
    SEQ_TRACE("[SEQ] alloc ok seq=0x%08lX%08lX\r\n",
              (unsigned long)(*seq_out>>32), (unsigned long)*seq_out);
    seq_unlock();
    return HAL_OK;
}

uint64_t storage_seq_get_high_watermark(void)
{
    return s_next_reserved;
}
