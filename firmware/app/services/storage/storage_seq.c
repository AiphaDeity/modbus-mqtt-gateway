/*
 * storage_seq.c — 序列号管理实现
 *
 * 持久化位置：Flash 0x040000 区（身份/元数据区）开头。
 * 布局：[magic(4B)][next_reserved(8B)][crc32(4B)]
 */

#include "storage_seq.h"
#include "flash_partition.h"
#include "spi_flash_w25q128.h"
#include <string.h>

#define SEQ_MAGIC        0x53455131U  /* "SEQ1" */
#define SEQ_FLASH_ADDR   FLASH_PART_IDENTITY_BASE
#define SEQ_STRUCT_SIZE  16U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint64_t next_reserved;
    uint32_t crc32;
} seq_meta_t;

static uint64_t s_next_reserved = 0;   /* 持久化的高水位 */
static uint64_t s_current       = 0;   /* 当前可用序号起点 */
static uint64_t s_window_end    = 0;   /* 当前窗口结束（不含） */
static bool     s_inited        = false;

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

/* 从 Flash 读取序列号元数据 */
static HAL_StatusTypeDef seq_read_meta(seq_meta_t *meta)
{
    return spi_flash_w25q128_read(SEQ_FLASH_ADDR, (uint8_t *)meta, sizeof(seq_meta_t));
}

/* 写入序列号元数据到 Flash（先擦后写） */
static HAL_StatusTypeDef seq_write_meta(const seq_meta_t *meta)
{
    HAL_StatusTypeDef st = spi_flash_w25q128_sector_erase(SEQ_FLASH_ADDR);
    if (st != HAL_OK) return st;
    return spi_flash_w25q128_write(SEQ_FLASH_ADDR, (const uint8_t *)meta,
                                    sizeof(seq_meta_t));
}

HAL_StatusTypeDef storage_seq_init(void)
{
    seq_meta_t meta;
    HAL_StatusTypeDef st = seq_read_meta(&meta);

    if (st == HAL_OK && meta.magic == SEQ_MAGIC) {
        uint32_t calc = crc32_calc((uint8_t *)&meta,
                                    sizeof(meta) - sizeof(uint32_t));
        if (calc == meta.crc32) {
            s_next_reserved = meta.next_reserved;
            s_current       = meta.next_reserved;
            s_window_end    = meta.next_reserved;
            s_inited        = true;
            return HAL_OK;
        }
    }

    /* 无有效元数据，从 0 开始 */
    s_next_reserved = 0;
    s_current       = 0;
    s_window_end    = 0;
    s_inited        = true;

    /* 持久化初始高水位 */
    seq_meta_t init_meta = {
        .magic = SEQ_MAGIC,
        .next_reserved = 0,
    };
    init_meta.crc32 = crc32_calc((uint8_t *)&init_meta,
                                  sizeof(init_meta) - sizeof(uint32_t));
    return seq_write_meta(&init_meta);
}

HAL_StatusTypeDef storage_seq_alloc(uint64_t *seq_out)
{
    if (seq_out == NULL || !s_inited) return HAL_ERROR;

    /* 窗口用完，预留下一批 */
    if (s_current >= s_window_end) {
        uint64_t new_reserved = s_next_reserved + SEQ_RESERVE_BATCH;

        /* 接近最大值，停止生成 */
        if (new_reserved > UINT64_MAX - SEQ_RESERVE_BATCH) {
            return HAL_ERROR;
        }

        /* 先持久化新高水位 */
        seq_meta_t meta = {
            .magic = SEQ_MAGIC,
            .next_reserved = new_reserved,
        };
        meta.crc32 = crc32_calc((uint8_t *)&meta,
                                 sizeof(meta) - sizeof(uint32_t));
        HAL_StatusTypeDef st = seq_write_meta(&meta);
        if (st != HAL_OK) return st;

        s_next_reserved = new_reserved;
        s_window_end    = new_reserved;
        /* s_current 不变，从上次窗口结束处继续 */
    }

    *seq_out = s_current++;
    return HAL_OK;
}

uint64_t storage_seq_get_high_watermark(void)
{
    return s_next_reserved;
}
