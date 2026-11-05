#ifndef STORAGE_CTRLLOG_H
#define STORAGE_CTRLLOG_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * 控制日志（双区定长槽，Flash 0x080000~0x180000，每区 512KiB）。
 *
 * 用途：持久化"控制执行意图"与"待输出事件"，重启后据此恢复去重表与未完成事件。
 *
 * 槽格式（512B）：
 *   偏移 0    magic(4)  = 'CTLG'
 *   偏移 4    format_version(2)
 *   偏移 6    stream(2)
 *   偏移 8    payload_len(2)
 *   偏移 10   flags(2)
 *   偏移 12   timestamp(4)      写入时刻的 Unix 时间（0=未知）
 *   偏移 16   seq(8)            全局单调序列号
 *   偏移 24   dedup_key(8)      去重键（0=不去重）
 *   偏移 32   payload(payload_len)   不透明字节，内容由上层定义
 *   偏移 32+len  crc32(4)       覆盖 header+payload
 *   偏移 508  commit(4)         提交尾标（最后写入）
 *
 * 双区策略：当前区写满后回收另一区（整区 64KB 块擦除），再切换过去继续写。
 * 回收期间 append 返回 HAL_BUSY。
 */

#define CTRLLOG_FORMAT_VERSION   1U
#define CTRLLOG_SLOT_SIZE        512U
#define CTRLLOG_PAYLOAD_MAX      472U

/* stream 语义（内容由上层定义，M3 只负责存取） */
#define CTRLLOG_STREAM_INTENT    0U   /* 控制执行意图 */
#define CTRLLOG_STREAM_OUTPUT    1U   /* 待输出事件 */
#define CTRLLOG_STREAM_GENERIC   2U

/* 记录视图（回调期间有效，payload 指向槽内缓冲区） */
typedef struct {
    uint16_t        format_version;
    uint16_t        stream;
    uint16_t        payload_len;
    uint16_t        flags;
    uint32_t        timestamp;
    uint64_t        seq;
    uint64_t        dedup_key;
    const uint8_t  *payload;
} ctrllog_record_t;

typedef void (*ctrllog_visit_fn)(const ctrllog_record_t *rec, void *user);

// 初始化：扫描双区，恢复上次写入位置与最近去重键
HAL_StatusTypeDef storage_ctrllog_init(void);

/*
 * 追加一条记录。
 * dedup_key != 0 且最近 128 条内已存在相同 (stream,dedup_key) 时跳过写入，
 * 并通过 is_duplicate 告知调用方（可为 NULL）。
 */
HAL_StatusTypeDef storage_ctrllog_append(uint16_t stream, uint64_t dedup_key,
                                         const void *payload, uint16_t len,
                                         bool *is_duplicate);

// 按写入顺序（旧→新）遍历有效记录
HAL_StatusTypeDef storage_ctrllog_foreach(ctrllog_visit_fn fn, void *user);

// 推进整区回收（在 M3 任务循环中周期性调用）
HAL_StatusTypeDef storage_ctrllog_maintenance(void);

// 最近一条已提交记录的序列号（无记录时为 0）
uint64_t storage_ctrllog_last_seq(void);

#endif /* STORAGE_CTRLLOG_H */
