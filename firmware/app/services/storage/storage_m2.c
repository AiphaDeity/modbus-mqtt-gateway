/*
 * storage_m2.c — M2↔M3 队列协议实现
 *
 * 队列全部由 M3 创建；Storage_Task 循环调用 storage_m2_process() 驱动：
 *   1) 写队列：M2 离线采集的记录 → storage_record_append（seq 由 M2 预先
 *      经 storage_seq_alloc 分配，payload 在本任务内立即拷入 576B 槽缓冲）
 *   2) 补传请求：填充静态应答缓冲（单在途，req_id 匹配），指针入应答队列
 *   3) 回执：ack 水位推进 → storage_record_ack_upto
 */

#include "storage_m2.h"
#include "storage_record.h"

#include <string.h>

/* ---------- 队列句柄 ---------- */

static osMessageQueueId_t s_write_q   = NULL;   /* 元素：storage_write_req_t*（M2 侧分配） */
static osMessageQueueId_t s_fetch_q   = NULL;   /* 元素：storage_fetch_req_t（值） */
static osMessageQueueId_t s_resp_q    = NULL;   /* 元素：storage_fetch_resp_t*（单静态缓冲） */
static osMessageQueueId_t s_ack_q     = NULL;   /* 元素：storage_ack_t（值） */

/* 应答缓冲：单在途请求；M2 收到指针后立即拷贝，再发下一个请求 */
static storage_fetch_resp_t s_resp_buf;

void storage_m2_init(void)
{
    osMessageQueueAttr_t attr = { 0 };
    attr.name = "m3_write_q";
    s_write_q = osMessageQueueNew(STORAGE_M2_WRITE_Q_DEPTH,
                                  sizeof(storage_write_req_t *), &attr);

    attr.name = "m3_fetch_q";
    s_fetch_q = osMessageQueueNew(STORAGE_M2_FETCH_Q_DEPTH,
                                  sizeof(storage_fetch_req_t), &attr);

    attr.name = "m3_resp_q";
    s_resp_q = osMessageQueueNew(1, sizeof(storage_fetch_resp_t *), &attr);

    attr.name = "m3_ack_q";
    s_ack_q = osMessageQueueNew(STORAGE_M2_ACK_Q_DEPTH,
                                sizeof(storage_ack_t), &attr);
}

osMessageQueueId_t storage_m2_write_queue(void)      { return s_write_q; }
osMessageQueueId_t storage_m2_fetch_req_queue(void)  { return s_fetch_q; }
osMessageQueueId_t storage_m2_fetch_resp_queue(void) { return s_resp_q;  }
osMessageQueueId_t storage_m2_ack_queue(void)        { return s_ack_q;   }

void storage_m2_get_cache_stats(storage_cache_stats_t *out)
{
    if (out == NULL) return;
    uint32_t pending = 0, dropped = 0;
    storage_record_stats(&pending, &dropped);
    /* 上报按 16 位截断（M2 的 status.cache_stats 字段宽度） */
    out->pending = (pending > 0xFFFFu) ? 0xFFFFu : (uint16_t)pending;
    out->dropped = (dropped > 0xFFFFu) ? 0xFFFFu : (uint16_t)dropped;
}

/* ---------- 内部处理 ---------- */

static void process_write_queue(void)
{
    storage_write_req_t *req;
    while (osMessageQueueGet(s_write_q, &req, NULL, 0) == osOK) {
        if (req == NULL) continue;
        /* payload 立即拷入 576B 槽缓冲并落盘，此后 M2 的缓冲即可复用 */
        (void)storage_record_append(req->kind, req->seq, req->captured_at_ms,
                                    req->payload, req->payload_len);
    }
}

static void process_fetch_queue(void)
{
    storage_fetch_req_t req;
    if (osMessageQueueGet(s_fetch_q, &req, NULL, 0) != osOK) return;

    uint16_t max_count = req.max_count;
    if (max_count > STORAGE_M2_FETCH_BATCH_MAX) max_count = STORAGE_M2_FETCH_BATCH_MAX;

    uint16_t count = 0;
    storage_record_fetch(max_count, s_resp_buf.records, &count);

    s_resp_buf.req_id = req.req_id;
    s_resp_buf.count  = count;

    /* 应答队列深度 1：上一份应答没被取走就丢弃（M2 会超时重试） */
    (void)osMessageQueuePut(s_resp_q, &(storage_fetch_resp_t *){ &s_resp_buf }, 0, 0);
}

static void process_ack_queue(void)
{
    storage_ack_t ack;
    while (osMessageQueueGet(s_ack_q, &ack, NULL, 0) == osOK) {
        storage_record_ack_upto(ack.ack_upto_seq);
    }
}

void storage_m2_process(void)
{
    if (s_write_q == NULL) return;   /* 未初始化 */
    process_write_queue();
    process_fetch_queue();
    process_ack_queue();
}
