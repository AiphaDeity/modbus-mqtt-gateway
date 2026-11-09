/**
 * @file telemetry_task.c
 * @brief M2 遥测上报任务：周期从 M1 拿 Modbus 采集结果，拼 JSON，投 M3 写队列
 *
 *  流程：
 *    每 2 秒：
 *      mb_for_each_valid_slave()  → 遍历在线从站
 *      mb_pointmap_parse_slave()  → M1 帮我们把 raw reg 换成物理量
 *      拼 JSON（cJSON 或 snprintf）
 *      storage_seq_alloc()         → M3 分配全局 seq
 *      osMessageQueuePut(write_q)  → 丢给 M3 离线缓存
 *
 *  注意：
 *    - 本任务不直接碰 MQTT netconn（那是 MqttClient_Task 独占）
 *    - 联网补传由后续单独的 replay 循环负责
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "cmsis_os2.h"

#include "mb_api.h"
#include "mb_pointmap.h"
#include "storage_m2.h"
#include "storage_seq.h"
#include "mqtt_client.h"
#include "app_modules.h"

/* 上报周期：Modbus 采集本身可能 1~2 秒一轮，这里 2 秒足够 */
#define TELEMETRY_PERIOD_MS   2000U

/* 单条 payload 上限：storage_write_req_t.payload[512] */
#define PAYLOAD_MAX           512U

/**
 * 遍历回调：处理一个从站，拼 JSON 并投写队列
 *
 *  注意：mb_for_each_valid_slave 内部已持有 xSlaveCfgMutex，
 *        这里不要调 mb_add_slave/mb_delete_slave/mb_take_slave_snapshot（会死锁）。
 *        mb_pointmap_parse_slave 内部自己拿数据锁，安全。
 */
static int on_slave_telemetry(mb_slave_runtime_t *slave, void *ctx)
{
    (void)ctx;

    /* 跳过没有有效数据的从站（刚上电或一直掉线） */
    if (!slave->data_valid || slave->valid_point == 0) return 0;

    /* 1) 让 M1 把 raw reg 解析成物理量（名称/单位/已乘缩放）。
     *    buf 和 pts 都用 static：只有 telemetry 一个任务调本函数，
     *    但栈上放 512B+ 缓冲会爆栈（snprintf %f 内部还要几百字节）。 */
    static mb_parsed_point_t pts[MB_POINTS_PER_SLAVE] __attribute__((section(".ccmram")));
    int npts = mb_pointmap_parse_slave(slave->telegram.u8id, pts);
    if (npts <= 0) return 0;   /* 没配点位表，跳过 */

    /* 2) 拼 JSON。示例：
     * {"g":"mb2mqtt_001","slave":1,"online":1,"pts":[{"n":"temp","v":25.3,"u":"C"},...]}
     */
    static char buf[PAYLOAD_MAX] __attribute__((section(".ccmram")));
    int n = snprintf(buf, sizeof(buf),
        "{\"g\":\"mb2mqtt_001\",\"slave\":%u,\"online\":%u,\"pts\":[",
        slave->telegram.u8id, slave->is_online);

    for (int i = 0; i < npts && npts < MB_POINTS_PER_SLAVE; i++)
    {
        if (!pts[i].valid) continue;
        /* 每个点位：{"n":"名称","v":12.34,"u":"单位"} */
        int w = snprintf(buf + n, sizeof(buf) - n,
            "%s{\"n\":\"%s\",\"v\":%.2f,\"u\":\"%s\"}",
            (n > 0 && buf[n-1] != '[') ? "," : "",
            pts[i].name, (double)pts[i].value, pts[i].unit);
        if (n + w >= (int)sizeof(buf) - 2) break;   /* 满了就截断 */
        n += w;
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");

    /* 3) 从 M3 取全局 seq 号 */
    uint64_t seq = 0;
    if (storage_seq_alloc(&seq) != HAL_OK)
    {
        printf("[TEL] seq_alloc failed, skip slave=%u\r\n", slave->telegram.u8id);
        return 0;
    }

    /* 4) 填写请求，丢给 M3。全部值拷贝，put 返回后 buf 可复用 */
    storage_write_req_t req = {
        .kind = 1,                     /* 1=telemetry */
        .seq = seq,
        .captured_at_ms = 0,            /* TODO: RTC 校时后填真实毫秒 */
        .payload_len = (uint16_t)n,
    };
    memcpy(req.payload, buf, n);

    osStatus_t st = osMessageQueuePut(storage_m2_write_queue(), &req, 0, 0);
    if (st != osOK)
    {
        /* 队列满（离线太久+补传卡住）：丢一条，M3 会维护 dropped 计数 */
        printf("[TEL] write queue full, drop seq=%lu slave=%u\r\n",
               (unsigned long)seq, slave->telegram.u8id);
    }
    else
    {
        /* 5) 在线时直接 PUBLISH 到 broker（不等补传），失败也不阻塞离线缓存 */
        int pr = MqttClient_Publish("mb2mqtt_001/telemetry",
                                   (const uint8_t *)buf, (size_t)n);
        /* 注：newlib-nano 不支持 %llu，seq 当前值 < 4G，用 %lu 打即可 */
        printf("[TEL] seq=%lu slave=%u online=%u pts=%d mqtt_pub=%d\r\n",
               (unsigned long)seq, (unsigned)slave->telegram.u8id,
               (unsigned)slave->is_online, npts, pr);
    }
    return 0;   /* 0=继续遍历下一个从站 */
}

/* 空 init：telemetry 没有自己的资源要初始化，全靠 M1/M3 已就绪 */
AppModuleInitResult Telemetry_Init(void)
{
    return APP_MODULE_INIT_OK;
}

void Telemetry_Task(void *argument)
{
    (void)argument;
    /* 等系统初始化完（DHCP/MQTT/Modbus 都起来）再开始 */
    osDelay(5000);

    for (;;)
    {
        mb_for_each_valid_slave(on_slave_telemetry, NULL);
        osDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}
