/*
 * storage_m2.c — M2↔M3 队列协议实现
 *
 * 队列全部由 M3 创建；Storage_Task 循环调用 storage_m2_process() 驱动：
 *   1) 写队列：M2 离线采集的记录 → storage_record_append（seq 由 M2 预先
 *      经 storage_seq_alloc 分配）
 *   2) 补传请求：填入静态应答缓冲后值拷贝进应答队列（单在途，req_id 匹配）
 *   3) 回执：ack 水位推进 → storage_record_ack_upto
 *
 * 全部消息为值传递：M2 put 返回后栈上缓冲即可复用，无生命周期约束。
 *
 * ★ 关键设计：四个队列的控制块 + buffer 全部用静态数组（.bss），
 *   不走 pvPortMalloc。否则 s_write_q（8×532B）+ s_resp_q（1×4.3KB）
 *   就吃 ~8.6KB heap，加上 M1/MQTT 资源和碎片，20KB heap 必然顶不住。
 */

#include "storage_m2.h"
#include "storage_record.h"
#include "storage_runtime.h"
#include "FreeRTOS.h"
#include "queue.h"

#include <string.h>
#include <stdio.h>

/* ---------- 队列句柄 ---------- */

static osMessageQueueId_t s_write_q   = NULL;   /* 元素：storage_write_req_t（536B 值） */
static osMessageQueueId_t s_fetch_q   = NULL;   /* 元素：storage_fetch_req_t（值） */
static osMessageQueueId_t s_resp_q    = NULL;   /* 元素：storage_fetch_resp_t（~4.3KB 值，深度 1） */
static osMessageQueueId_t s_ack_q     = NULL;   /* 元素：storage_ack_t（值） */
/* 写入失败可能已推进介质状态，锁存后保留原队首，不自动重试冒充重复成功。 */
static HAL_StatusTypeDef s_write_error;

/* CCM 队列和响应缓冲仅由 CPU 访问，启动时统一清零，不可用于 DMA。 */
#define M3_CCM  __attribute__((section(".bss.ccmram"), used))

/* 应答组装缓冲：填好后值拷贝入队列，随后即可重填 */
M3_CCM static storage_fetch_resp_t s_resp_buf;

/*
 * 每个 queue 的静态存储：size 按 "控制块(≈128B) + 对齐(4) + depth×item_size"
 * 估算，用 _Alignas(8) 对齐到 8 字节（FreeRTOS port 要求）。
 */
#define QCB_SIZE(depth, item)  (128U + ((depth) * (item)))
_Static_assert(sizeof(StaticQueue_t) <= 128U, "M3 queue control reservation");

_Alignas(8) M3_CCM static uint8_t s_write_q_cb[ QCB_SIZE(STORAGE_M2_WRITE_Q_DEPTH,
                                                  sizeof(storage_write_req_t)) ];
_Alignas(8) M3_CCM static uint8_t s_fetch_q_cb[ QCB_SIZE(STORAGE_M2_FETCH_Q_DEPTH,
                                                  sizeof(storage_fetch_req_t)) ];
_Alignas(8) M3_CCM static uint8_t s_resp_q_cb [ QCB_SIZE(1,
                                                  sizeof(storage_fetch_resp_t)) ];
_Alignas(8) M3_CCM static uint8_t s_ack_q_cb  [ QCB_SIZE(STORAGE_M2_ACK_Q_DEPTH,
                                                  sizeof(storage_ack_t)) ];

void storage_m2_init(void)
{
    /* 建队列前清理静态存储和响应缓冲。 */
    memset(&s_resp_buf,   0, sizeof(s_resp_buf));
    memset(s_write_q_cb,  0, sizeof(s_write_q_cb));
    memset(s_fetch_q_cb,  0, sizeof(s_fetch_q_cb));
    memset(s_resp_q_cb,   0, sizeof(s_resp_q_cb));
    memset(s_ack_q_cb,    0, sizeof(s_ack_q_cb));

    static const osMessageQueueAttr_t attr_write = {
        .name = "m3_write_q", .mq_mem = s_write_q_cb + 128U, .mq_size = sizeof(s_write_q_cb) - 128U,
        .cb_mem = s_write_q_cb, .cb_size = sizeof(s_write_q_cb) };
    static const osMessageQueueAttr_t attr_fetch = {
        .name = "m3_fetch_q", .mq_mem = s_fetch_q_cb + 128U, .mq_size = sizeof(s_fetch_q_cb) - 128U,
        .cb_mem = s_fetch_q_cb, .cb_size = sizeof(s_fetch_q_cb) };
    static const osMessageQueueAttr_t attr_resp = {
        .name = "m3_resp_q",  .mq_mem = s_resp_q_cb + 128U, .mq_size = sizeof(s_resp_q_cb) - 128U,
        .cb_mem = s_resp_q_cb,  .cb_size = sizeof(s_resp_q_cb) };
    static const osMessageQueueAttr_t attr_ack = {
        .name = "m3_ack_q",   .mq_mem = s_ack_q_cb + 128U, .mq_size = sizeof(s_ack_q_cb) - 128U,
        .cb_mem = s_ack_q_cb,   .cb_size = sizeof(s_ack_q_cb) };

    s_write_q = osMessageQueueNew(STORAGE_M2_WRITE_Q_DEPTH,
                                  sizeof(storage_write_req_t), &attr_write);
    s_fetch_q = osMessageQueueNew(STORAGE_M2_FETCH_Q_DEPTH,
                                  sizeof(storage_fetch_req_t), &attr_fetch);
    s_resp_q  = osMessageQueueNew(1, sizeof(storage_fetch_resp_t), &attr_resp);
    s_ack_q   = osMessageQueueNew(STORAGE_M2_ACK_Q_DEPTH,
                                  sizeof(storage_ack_t), &attr_ack);
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

/* 先查看队首，只有记录写入明确成功后才移除请求。 */
static HAL_StatusTypeDef process_write_queue(void)
{
    storage_write_req_t req;
    if (s_write_error != HAL_OK) return s_write_error;
    if (xQueuePeek((QueueHandle_t)s_write_q, &req, 0U) != pdPASS) return HAL_OK;
    HAL_StatusTypeDef result = storage_record_append(req.kind, req.seq, req.captured_at_ms,
                                                    req.payload, req.payload_len);
    if (result == HAL_OK) {
        if (osMessageQueueGet(s_write_q, &req, NULL, 0U) != osOK) result = HAL_ERROR;
    }
    if (result == HAL_ERROR || result == HAL_TIMEOUT) {
        s_write_error = result;
        printf("[M3] queued write unresolved seq=%08lX%08lX status=%u\r\n",
               (unsigned long)(req.seq >> 32), (unsigned long)req.seq, (unsigned)result);
    }
    return result;
}

/* 请求、读回及应答成对处理；应答满或读取失败时不移除请求。 */
static HAL_StatusTypeDef process_fetch_queue(void)
{
    storage_fetch_req_t req;
    if (xQueuePeek((QueueHandle_t)s_fetch_q, &req, 0U) != pdPASS) return HAL_OK;
    if (osMessageQueueGetSpace(s_resp_q) == 0U) return HAL_BUSY;

    uint16_t max_count = req.max_count;
    if (max_count > STORAGE_M2_FETCH_BATCH_MAX) max_count = STORAGE_M2_FETCH_BATCH_MAX;

    uint16_t count = 0;
    HAL_StatusTypeDef result = storage_record_fetch(max_count, s_resp_buf.records, &count);
    if (result != HAL_OK) return result;

    s_resp_buf.req_id = req.req_id;
    s_resp_buf.count  = count;

    if (osMessageQueuePut(s_resp_q, &s_resp_buf, 0, 0) != osOK) return HAL_ERROR;
    return osMessageQueueGet(s_fetch_q, &req, NULL, 0U) == osOK ? HAL_OK : HAL_ERROR;
}

/* 保留不能应用的回执；水位更新后的持久化由统一同步阶段确认。 */
static HAL_StatusTypeDef process_ack_queue(void)
{
    storage_ack_t ack;
    if (xQueuePeek((QueueHandle_t)s_ack_q, &ack, 0U) != pdPASS) return HAL_OK;
    if (!storage_mutation_allowed() || !storage_record_is_ready()) return HAL_BUSY;
    storage_record_ack_upto(ack.ack_upto_seq);
    return osMessageQueueGet(s_ack_q, &ack, NULL, 0U) == osOK ? HAL_OK : HAL_ERROR;
}

/* 持完整操作锁收尾一轮；临时忙与明确I/O失败分别返回。 */
HAL_StatusTypeDef storage_m2_process(void)
{
    if (s_write_q == NULL || s_fetch_q == NULL || s_resp_q == NULL || s_ack_q == NULL)
        return HAL_ERROR;
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef write_result = process_write_queue();
    HAL_StatusTypeDef fetch_result = process_fetch_queue();
    HAL_StatusTypeDef ack_result = process_ack_queue();
    storage_mutation_end();
    if (write_result == HAL_ERROR || fetch_result == HAL_ERROR || ack_result == HAL_ERROR)
        return HAL_ERROR;
    if (write_result == HAL_TIMEOUT || fetch_result == HAL_TIMEOUT) return HAL_TIMEOUT;
    return write_result == HAL_BUSY || fetch_result == HAL_BUSY || ack_result == HAL_BUSY ? HAL_BUSY : HAL_OK;
}

/* 调用方已暂停生产和业务处理，只观察未完成请求，不清空或伪造应答。 */
bool storage_m2_settled(void)
{
    return s_write_error == HAL_OK &&
           s_write_q != NULL && s_ack_q != NULL && s_fetch_q != NULL && s_resp_q != NULL &&
           osMessageQueueGetCount(s_write_q) == 0U &&
           osMessageQueueGetCount(s_ack_q) == 0U &&
           osMessageQueueGetCount(s_fetch_q) == 0U;
}
