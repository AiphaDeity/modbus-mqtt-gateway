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

/* ---------- 队列句柄 ---------- */

static osMessageQueueId_t s_write_q   = NULL;   /* 元素：storage_write_req_t（536B 值） */
static osMessageQueueId_t s_fetch_q   = NULL;   /* 元素：storage_fetch_req_t（值） */
static osMessageQueueId_t s_resp_q    = NULL;   /* 元素：storage_fetch_resp_t（~4.3KB 值，深度 1） */
static osMessageQueueId_t s_ack_q     = NULL;   /* 元素：storage_ack_t（值） */

/*
 * ============================================================================
 * ★ RAM 优化：以下 5 块存储合计约 13.3KB，整体搬到 CCMRAM。
 *
 *   F407 有 64KB CCM（0x10000000），只能被 CPU 访问、不能 DMA。本工程
 *   此前 CCM 占用为 0，完全空着。这 5 块是纯软件队列存储——只被
 *   osMessageQueuePut/Get 的内核 memcpy 读写，不涉及任何 DMA，
 *   所以放进 CCM 对功能零影响，却能把 13.3KB 从 128KB SRAM 里让出来。
 *
 *   ⚠️ 本工程 startup_stm32f407xx.s 没有实现 .ccmram 段的拷贝/清零循环，
 *      CCM 里的变量上电是随机值，因此必须在 storage_m2_init() 里手动 memset。
 *      任意一块漏清零 → 队列控制块带垃圾值 → osMessageQueueNew 行为异常。
 * ============================================================================
 */
#define M3_CCM  __attribute__((section(".ccmram"), used))

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
    /*
     * CCM 段不被 startup 清零（见上方说明），这里必须逐块手动清零，
     * 否则队列控制块和存储区带随机值。
     */
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

static void process_write_queue(void)
{
    storage_write_req_t req;
    /* 每轮至多一条，持续上报不能饿死升级读回和取消请求。 */
    if (osMessageQueueGet(s_write_q, &req, NULL, 0) == osOK) {
        (void)storage_record_append(req.kind, req.seq, req.captured_at_ms,
                                    req.payload, req.payload_len);
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

    /* 应答队列深度 1：上一份应答没被取走就丢弃（M2 单在途 + 超时重试） */
    (void)osMessageQueuePut(s_resp_q, &s_resp_buf, 0, 0);
}

static void process_ack_queue(void)
{
    storage_ack_t ack;
    if (osMessageQueueGet(s_ack_q, &ack, NULL, 0) == osOK) {
        storage_record_ack_upto(ack.ack_upto_seq);
    }
}

void storage_m2_process(void)
{
    if (s_write_q == NULL || !storage_mutation_allowed()) return;
    process_write_queue();
    process_fetch_queue();
    process_ack_queue();
}

/* 调用方已暂停生产和业务处理，只观察未完成请求，不清空或伪造应答。 */
bool storage_m2_settled(void)
{
    return s_write_q != NULL && s_ack_q != NULL && s_fetch_q != NULL && s_resp_q != NULL &&
           osMessageQueueGetCount(s_write_q) == 0U &&
           osMessageQueueGetCount(s_ack_q) == 0U &&
           osMessageQueueGetCount(s_fetch_q) == 0U;
}
