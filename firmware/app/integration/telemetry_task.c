/**
 * @file telemetry_task.c
 * @brief M2 遥测上报任务：周期从 M1 拿 Modbus 采集结果，拼 JSON，投 M3 写队列
 *
 *  ★★两阶段模式（Modbus_Interface_Guide 第 6.2 节）：
 *    mb_for_each_valid_slave 遍历期间持有 xSlaveCfgMutex（非递归），
 *    回调里不能调 mb_pointmap_parse_slave / mb_take_slave_snapshot（会自死锁）。
 *    所以：
 *      阶段1：遍历只收集 slave_id 列表（回调里只读 telegram.u8id）
 *      阶段2：退出遍历后（锁已释放），逐个 id 调 mb_take_slave_snapshot + mb_pointmap_parse_slave
 *
 *  流程：
 *    每 2 秒：
 *      mb_for_each_valid_slave(collect_ids)  → 收集有效从站 ID
 *      for each id:
 *        mb_take_slave_snapshot(id)          → 取数据快照（检查 data_valid）
 *        mb_pointmap_parse_slave(id)         → M1 把 raw reg 换成物理量
 *        拼 JSON
 *        storage_seq_alloc()                 → M3 分配全局 seq
 *        osMessageQueuePut(write_q)          → 丢给 M3 离线缓存
 *        MqttClient_Publish()                → 在线时直接发
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
#if GATEWAY_ENABLE_M2
#include "network_upgrade.h" /* NetworkUpgrade_IsQuiesced() 升级门控 */
#endif

/* 上报周期：Modbus 采集本身可能 1~2 秒一轮，这里 2 秒足够 */
#define TELEMETRY_PERIOD_MS   2000U

/* 单条 payload 上限：storage_write_req_t.payload[512] */
#define PAYLOAD_MAX           512U

/* 最多收集的从站 ID 数（MB_SLAVE_HARD_MAX=16） */
#define TEL_MAX_SLAVES        16U

/* ========= 阶段1：收集有效从站 ID（回调里只读 ID，不调任何加锁函数）========= */

typedef struct
{
    uint8_t ids[TEL_MAX_SLAVES];
    uint8_t n;
} tel_slave_list_t;

static int collect_slave_ids(mb_slave_runtime_t *slave, void *ctx)
{
    tel_slave_list_t *list = (tel_slave_list_t *)ctx;
    if (list->n < TEL_MAX_SLAVES)
    {
        /* 只记 ID，不读数据字段（数据字段在阶段2用 snapshot 安全读取） */
        list->ids[list->n++] = slave->telegram.u8id;
    }
    return 0;   /* 继续遍历 */
}

/* ========= 阶段2：处理单个从站（锁已释放，可调任意 pull 接口）========= */

static void publish_one_slave(uint8_t slave_id)
{
    /* 1) 取快照：内部加 xSlaveDataMutex，安全读 reg_data_buf / data_valid */
    mb_slave_snapshot_t snap;
    if (mb_take_slave_snapshot(slave_id, &snap) != MB_STATUS_OK)
        return;

    if (!snap.data_valid || snap.valid_point == 0)
    {
        printf("[TEL] skip slave=%u dv=%u vp=%u\r\n",
               (unsigned)slave_id, (unsigned)snap.data_valid,
               (unsigned)snap.valid_point);
        return;
    }

    /* 2) 解析点位表：内部加 xSlaveCfgMutex 定位 + xSlaveDataMutex 读 raw reg，
     *    此时本任务不持有任何 Modbus 锁，安全 */
    static mb_parsed_point_t pts[MB_POINTS_PER_SLAVE] __attribute__((section(".ccmram")));
    int npts = mb_pointmap_parse_slave(slave_id, pts);
    if (npts <= 0)
    {
        printf("[TEL] no points slave=%u\r\n", (unsigned)slave_id);
        return;
    }

    /* 3) 拼 JSON：{"g":"mb2mqtt_001","slave":2,"online":1,"pts":[{"n":"temp","v":25.3,"u":"C"},...]} */
    static char buf[PAYLOAD_MAX] __attribute__((section(".ccmram")));
    int n = snprintf(buf, sizeof(buf),
        "{\"g\":\"mb2mqtt_001\",\"slave\":%u,\"online\":%u,\"pts\":[",
        (unsigned)slave_id, (unsigned)snap.is_online);

    for (int i = 0; i < npts; i++)
    {
        if (!pts[i].valid) continue;
        int w = snprintf(buf + n, sizeof(buf) - n,
            "%s{\"n\":\"%s\",\"v\":%.2f,\"u\":\"%s\"}",
            (n > 0 && buf[n-1] != '[') ? "," : "",
            pts[i].name, (double)pts[i].value, pts[i].unit);
        if (n + w >= (int)sizeof(buf) - 2) break;
        n += w;
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");

    /* 4) 从 M3 取全局 seq 号 */
    uint64_t seq = 0;
    if (storage_seq_alloc(&seq) != HAL_OK)
    {
        printf("[TEL] seq_alloc failed, skip slave=%u\r\n", (unsigned)slave_id);
        return;
    }

    /* 5) 填写请求，丢给 M3 离线缓存 */
    storage_write_req_t req = {
        .kind = 1,
        .seq = seq,
        .captured_at_ms = 0,
        .payload_len = (uint16_t)n,
    };
    memcpy(req.payload, buf, n);

    osStatus_t st = osMessageQueuePut(storage_m2_write_queue(), &req, 0, 0);
    if (st != osOK)
    {
        printf("[TEL] write queue full, skip cache slave=%u\r\n", (unsigned)slave_id);
    }

    /* 6) 在线时直接 PUBLISH（不管队列是否满，队列只是离线备份） */
    printf("[TEL-DATA] %.*s\r\n", (int)n, buf);
    int pr = MqttClient_Publish(MqttClient_TopicTelemetry(),
                               (const uint8_t *)buf, (size_t)n);
    printf("[TEL] seq=%lu slave=%u online=%u pts=%d mqtt_pub=%d\r\n",
           (unsigned long)seq, (unsigned)slave_id,
           (unsigned)snap.is_online, npts, pr);
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
    printf("[TEL] task started\r\n");

    for (;;)
    {
#if GATEWAY_ENABLE_M2
        /* 升级门控：被暂停时跳过采集+上报，只等恢复（保留任务存活） */
        if (NetworkUpgrade_IsQuiesced())
        {
            osDelay(pdMS_TO_TICKS(500));
            continue;
        }
#endif

        /* 阶段1：收集有效从站 ID（只持 cfg 锁很短时间） */
        tel_slave_list_t list = {0};
        uint8_t cnt = mb_for_each_valid_slave(collect_slave_ids, &list);

        /* 阶段2：逐个从站处理（不持有任何 Modbus 锁） */
        for (uint8_t i = 0; i < list.n; i++)
            publish_one_slave(list.ids[i]);

        printf("[TEL] cycle done: %u slaves\r\n", (unsigned)cnt);
        osDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}
