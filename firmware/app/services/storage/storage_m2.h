#ifndef STORAGE_M2_H
#define STORAGE_M2_H

#include "cmsis_os2.h"
#include "storage_record.h"
#include <stdint.h>

/*
 * M2（MQTT 上云）↔ M3（存储）队列协议
 * 对齐《M2→M3 存储接口约定 v0.1》第 4 节：
 *   - 所有队列由 M3 创建并持有，M2 通过下面的 getter 拿句柄
 *   - 全部消息为值传递（osMessageQueuePut/Get 内核拷贝）：M2 用栈上或
 *     临时变量填好结构体，put 返回后即可复用，无生命周期约束
 *   - 补传为单在途请求（req_id 匹配）：M2 发出一个请求须等收到应答后
 *     再发下一个
 */

#define STORAGE_M2_WRITE_Q_DEPTH    8
#define STORAGE_M2_FETCH_Q_DEPTH    4
#define STORAGE_M2_ACK_Q_DEPTH      8
#define STORAGE_M2_FETCH_BATCH_MAX  8

/* 4.1 M2 → M3：写入一条待持久化记录（离线缓存）。值传递，put 即拷贝 */
typedef struct {
    uint8_t  kind;            /* 1=telemetry 2=event */
    uint64_t seq;             /* M2 经 storage_seq_alloc 分配，单调递增 */
    uint64_t captured_at_ms;  /* Unix 毫秒；未校时 = 0 */
    uint16_t payload_len;     /* ≤512 */
    uint8_t  payload[512];    /* JSON 原文 */
} storage_write_req_t;

/* 4.2 M2 → M3：补传拉取请求 */
typedef struct {
    uint32_t req_id;                /* M2 生成，匹配请求/应答 */
    uint16_t max_count;             /* ≤8 */
} storage_fetch_req_t;

/* 4.3 M3 → M2：补传数据应答（单在途，req_id 匹配） */
typedef struct {
    uint32_t     req_id;
    uint16_t     count;             /* 0 = 没有更多历史记录 */
    record_out_t records[STORAGE_M2_FETCH_BATCH_MAX];
} storage_fetch_resp_t;

/* 4.4 M2 → M3：上传成功回执（按 seq 水位删除，取本批最大 seq） */
typedef struct {
    uint64_t ack_upto_seq;
} storage_ack_t;

/* 4.5 缓存统计（M2 填 status.cache_stats） */
typedef struct {
    uint16_t pending;   /* 未补传条数 */
    uint16_t dropped;   /* 环覆盖/写失败丢失条数 */
} storage_cache_stats_t;

/* 初始化：创建全部队列（Storage_Task 启动时调用一次） */
void storage_m2_init(void);

/* M2 侧获取队列句柄（M2 在自己的任务里 osMessageQueuePut） */
osMessageQueueId_t storage_m2_write_queue(void);
osMessageQueueId_t storage_m2_fetch_req_queue(void);
osMessageQueueId_t storage_m2_fetch_resp_queue(void);
osMessageQueueId_t storage_m2_ack_queue(void);

/* M2 侧读取缓存统计（任意任务、随时可调） */
void storage_m2_get_cache_stats(storage_cache_stats_t *out);

/* Storage_Task 循环调用：处理写队列 → 补传请求 → 回执 */
void storage_m2_process(void);

#endif /* STORAGE_M2_H */
