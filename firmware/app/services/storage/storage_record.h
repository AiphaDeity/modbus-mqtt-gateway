#ifndef STORAGE_RECORD_H
#define STORAGE_RECORD_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * TF 卡记录持久化（FatFs，段文件 4MiB × 48 个，共 192MiB）。
 *
 * 设计：
 *   - 段文件：seg_0000.bin ~ seg_0047.bin，目录 /records
 *   - 每段 4MiB，512B 定长槽 → 8192 槽/段
 *   - 循环覆盖：48 段用完后，打开下一段时删除最旧段
 *   - 记录格式与控制日志一致（不透明 payload + 提交尾标防半成品）
 *
 * 记录槽（512B）：
 *   偏移 0    magic(4)  = 'FRMT'
 *   偏移 4    format_version(2)
 *   偏移 6    stream(2)
 *   偏移 8    payload_len(2)
 *   偏移 10   flags(2)
 *   偏移 12   timestamp(4)
 *   偏移 16   seq(8)
 *   偏移 24   reserved(8)
 *   偏移 32   payload(payload_len)    不透明，最大 472B
 *   偏移 32+len  crc32(4)
 *   偏移 508  commit(4)  最后写入
 *
 * 注意：记录格式与控制日志类似，但 magic/reserved 不同，便于区分。
 */

#define RECORD_FORMAT_VERSION    1U
#define RECORD_SLOT_SIZE         512U
#define RECORD_PAYLOAD_MAX       472U
#define RECORD_SEGMENT_SIZE      (4U * 1024U * 1024U)  /* 4 MiB */
#define RECORD_SEGMENT_SLOTS     (RECORD_SEGMENT_SIZE / RECORD_SLOT_SIZE)  /* 8192 */
#define RECORD_SEGMENT_COUNT     48U

/* stream 定义（与控制日志对齐） */
#define RECORD_STREAM_GENERIC    0U
#define RECORD_STREAM_DATA       1U   /* 采集数据历史 */
#define RECORD_STREAM_EVENT      2U   /* 事件日志 */

typedef struct {
    uint16_t        format_version;
    uint16_t        stream;
    uint16_t        payload_len;
    uint16_t        flags;
    uint32_t        timestamp;
    uint64_t        seq;
    const uint8_t  *payload;
} record_view_t;

typedef void (*record_visit_fn)(const record_view_t *rec, void *user);

/* 初始化：挂载 SD 卡，创建 /records 目录（若不存在），扫描现有段文件 */
HAL_StatusTypeDef storage_record_init(void);

/* 追加一条记录；满段时自动打开下一段并在需要时删除最旧段 */
HAL_StatusTypeDef storage_record_append(uint16_t stream,
                                       const void *payload, uint16_t len);

/* 按写入顺序（旧→新）遍历所有有效记录 */
HAL_StatusTypeDef storage_record_foreach(record_visit_fn fn, void *user);

/* 最近一条已提交记录的序列号（无记录时返回 0） */
uint64_t storage_record_last_seq(void);

/* 是否已就绪（卡挂载成功） */
bool storage_record_is_ready(void);

#endif /* STORAGE_RECORD_H */
