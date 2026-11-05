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

#define SEQ_MAGIC   0x53455131U  /* "SEQ1" */

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

static HAL_StatusTypeDef copy_write(int idx, uint64_t next_reserved)
{
    seq_meta_t meta;
    meta.magic         = SEQ_MAGIC;
    meta.next_reserved = next_reserved;
    meta.crc32         = crc32_calc((const uint8_t *)&meta, SEQ_CRC_LEN);

    HAL_StatusTypeDef st = spi_flash_w25q128_sector_erase(s_copy_addr[idx]);
    if (st != HAL_OK) return st;

    return spi_flash_w25q128_write(s_copy_addr[idx], (const uint8_t *)&meta,
                                    sizeof(meta));
}

HAL_StatusTypeDef storage_seq_init(void)
{
    seq_meta_t meta_a, meta_b;
    bool ok_a, ok_b;

    if (!seq_mutex_init()) return HAL_ERROR;

    seq_lock();

    ok_a = copy_read(0, &meta_a);
    ok_b = copy_read(1, &meta_b);

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
    s_inited     = true;

    /* 空设备：把初始高水位落下，避免下一次写入时两份都为空 */
    if (s_newer < 0) {
        HAL_StatusTypeDef st = copy_write(0, SEQ_FIRST_VALUE);
        if (st != HAL_OK) {
            seq_unlock();
            return st;
        }
        s_newer = 0;
    }

    seq_unlock();
    return HAL_OK;
}

HAL_StatusTypeDef storage_seq_alloc(uint64_t *seq_out)
{
    HAL_StatusTypeDef st;

    if (seq_out == NULL || !s_inited) return HAL_ERROR;

    seq_lock();

    /* 窗口用完，预留下一批 */
    if (s_current >= s_window_end) {
        uint64_t new_reserved = s_next_reserved + SEQ_RESERVE_BATCH;

        /* 接近最大值则停止生成，不回绕 */
        if (new_reserved < s_next_reserved ||
            new_reserved > UINT64_MAX - SEQ_RESERVE_BATCH) {
            seq_unlock();
            return HAL_ERROR;
        }

        /* 先持久化新高水位：覆盖较旧副本 */
        int target = (s_newer < 0) ? 0 : (1 - s_newer);
        st = copy_write(target, new_reserved);
        if (st != HAL_OK) {
            seq_unlock();
            return st;
        }

        /* 回读校验，失败则不发放新窗口内的号 */
        seq_meta_t verify;
        if (!copy_read(target, &verify) || verify.next_reserved != new_reserved) {
            seq_unlock();
            return HAL_ERROR;
        }

        s_newer         = target;
        s_next_reserved = new_reserved;
        s_window_end    = new_reserved;
    }

    *seq_out = s_current++;
    seq_unlock();
    return HAL_OK;
}

uint64_t storage_seq_get_high_watermark(void)
{
    return s_next_reserved;
}
