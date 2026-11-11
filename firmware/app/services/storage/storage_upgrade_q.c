/*
 * storage_upgrade_q.c — 升级请求/应答队列
 *
 * 深度1、值拷贝，沿用req_id和迟到应答替换机制。
 * 队列控制块及消息存储显式静态分配，不消耗运行期堆。
 */

#include "storage_upgrade_q.h"
#include "storage_upgrade.h"
#include "FreeRTOS.h"
#include "queue.h"

#include <stdio.h>

static osMessageQueueId_t s_req_q;
static osMessageQueueId_t s_rsp_q;
static StaticQueue_t s_req_cb;
static StaticQueue_t s_rsp_cb;
_Alignas(8) static uint8_t s_req_mem[sizeof(UpgradeStorageRequest)];
_Alignas(8) static uint8_t s_rsp_mem[sizeof(UpgradeStorageResponse)];

/* 真实队列必须同时提供控制块及消息区，任一创建失败均不宣布就绪。 */
bool storage_upgrade_q_init(void)
{
    static const osMessageQueueAttr_t req_attr = {
        .name = "m4_upg_req", .cb_mem = &s_req_cb, .cb_size = sizeof(s_req_cb),
        .mq_mem = s_req_mem, .mq_size = sizeof(s_req_mem)
    };
    static const osMessageQueueAttr_t rsp_attr = {
        .name = "m4_upg_rsp", .cb_mem = &s_rsp_cb, .cb_size = sizeof(s_rsp_cb),
        .mq_mem = s_rsp_mem, .mq_size = sizeof(s_rsp_mem)
    };
    if (s_req_q == NULL) s_req_q = osMessageQueueNew(1U, sizeof(UpgradeStorageRequest), &req_attr);
    if (s_rsp_q == NULL) s_rsp_q = osMessageQueueNew(1U, sizeof(UpgradeStorageResponse), &rsp_attr);
    if (s_req_q == NULL || s_rsp_q == NULL) {
        printf("[UPGQ] queue create FAILED req=%p rsp=%p\r\n", (void *)s_req_q, (void *)s_rsp_q);
        return false;
    }
    return true;
}

/* 返回共同协议的请求队列，初始化失败时为NULL。 */
osMessageQueueId_t storage_upgrade_req_queue(void) { return s_req_q; }
/* 返回对应的值拷贝应答队列，不能与旧协议混用。 */
osMessageQueueId_t storage_upgrade_rsp_queue(void) { return s_rsp_q; }

/* 不等待消费者；挤掉已迟到的旧应答，仍由客户端匹配完整请求身份。 */
static void upg_rsp_push(const UpgradeStorageResponse *rsp)
{
    if (osMessageQueuePut(s_rsp_q, rsp, 0U, 0U) == osOK) return;
    UpgradeStorageResponse stale;
    (void)osMessageQueueGet(s_rsp_q, &stale, NULL, 0U);
    if (osMessageQueuePut(s_rsp_q, rsp, 0U, 0U) != osOK)
        printf("[UPGQ] response LOST req=%lu result=%ld\r\n",
               (unsigned long)rsp->req_id, (long)rsp->result);
}

/* 一次只执行一个请求，避免连续下载饿死控制结果和记录服务。 */
void storage_upgrade_q_process(uint32_t wait_ticks)
{
    if (s_req_q == NULL || s_rsp_q == NULL) return;
    UpgradeStorageRequest req;
    if (osMessageQueueGet(s_req_q, &req, NULL, wait_ticks) != osOK) return;
    UpgradeStorageResponse rsp;
    storage_upgrade_handle(&req, &rsp);
    upg_rsp_push(&rsp);
}
