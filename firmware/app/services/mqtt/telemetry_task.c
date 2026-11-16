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
 *        storage_seq_alloc()                 → M3 分配全局 seq
 *        osMessageQueuePut(write_q)          → 丢给 M3 离线缓存
 *        MQTT从已提交记录统一发送，实时与补传使用同一record_id
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"

#include "mb_api.h"
#include "mb_pointmap.h"
#include "storage_m2.h"
#include "storage_seq.h"
#include "storage_identity.h"
#include "storage_time.h"
#include "mqtt_client.h"
#include "app_modules.h"
#include "app_monitor.h"
#include "upgrade_quiesce.h"

/* 上报周期：Modbus 采集本身可能 1~2 秒一轮，这里 2 秒足够 */
#define TELEMETRY_PERIOD_MS   2000U

/* 最多收集的从站 ID 数（MB_SLAVE_HARD_MAX=16） */
#define TEL_MAX_SLAVES        16U

static volatile bool s_pause_requested;
static bool s_pause_acknowledged;

/* 请求方只能撤销旧确认，停稳确认由Telemetry任务自身更新。 */
void Telemetry_SetPaused(bool paused)
{
    taskENTER_CRITICAL();
    s_pause_requested = paused;
    s_pause_acknowledged = false;
    taskEXIT_CRITICAL();
}

/* 查询本次暂停请求对应的实际任务确认。 */
bool Telemetry_IsPaused(void)
{
    taskENTER_CRITICAL();
    bool paused = s_pause_requested && s_pause_acknowledged;
    taskEXIT_CRITICAL();
    return paused;
}

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

/* 完成一次从站采集快照、缓存和发送；false表示监督期限已失效。 */
static bool publish_one_slave(uint8_t slave_id)
{
    /* 1) 取快照：内部加 xSlaveDataMutex，安全读 reg_data_buf / data_valid */
    mb_slave_snapshot_t snap;
    if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 3000U)) return false;
    mb_status_e snapshot_status = mb_take_slave_snapshot(slave_id, &snap);
    if (!Monitor_ReportProgress(APP_MODULE_TELEMETRY, true)) return false;
    if (snapshot_status != MB_STATUS_OK)
        return true;

    if (!snap.data_valid || snap.valid_point == 0)
    {
        printf("[TEL] skip slave=%u dv=%u vp=%u\r\n",
               (unsigned)slave_id, (unsigned)snap.data_valid,
               (unsigned)snap.valid_point);
        return true;
    }

    /* 2) 解析点位表：内部加 xSlaveCfgMutex 定位 + xSlaveDataMutex 读 raw reg，
     *    此时本任务不持有任何 Modbus 锁，安全 */
    static mb_parsed_point_t pts[MB_POINTS_PER_SLAVE] __attribute__((section(".bss.ccmram")));
    if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 3000U)) return false;
    int npts = mb_pointmap_parse_slave(slave_id, pts);
    if (!Monitor_ReportProgress(APP_MODULE_TELEMETRY, true)) return false;
    if (npts <= 0)
    {
        printf("[TEL] no points slave=%u\r\n", (unsigned)slave_id);
        return true;
    }

    char gateway[GATEWAY_ID_MAX_LEN];
    if (!MqttClient_GetGatewayId(gateway, sizeof(gateway)))
        return true;
    for (const char *c = gateway; *c; c++)
        if ((unsigned char)*c < 0x20U || *c == '"' || *c == '\\') return true;

    if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 3000U)) return false;
    uint64_t captured_at_ms = (uint64_t)storage_get_unix_time() * 1000U;
    if (!Monitor_ReportProgress(APP_MODULE_TELEMETRY, true)) return false;

    // 直接使用原存储请求编码；装不下的点继续写下一条，不截断点表。
    for (int first = 0; first < npts; )
    {
        while (first < npts && !pts[first].valid) first++;
        if (first == npts) break;
        uint64_t seq = 0;
        if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 3000U)) return false;
        HAL_StatusTypeDef sequence_status = storage_seq_alloc(&seq);
        if (!Monitor_ReportProgress(APP_MODULE_TELEMETRY, true)) return false;
        if (sequence_status != HAL_OK) return true;

        storage_write_req_t req = {.kind = RECORD_KIND_TELEMETRY, .seq = seq,
                                   .captured_at_ms = captured_at_ms};
        char *buf = (char *)req.payload;
        int n = snprintf(buf, sizeof(req.payload),
            "{\"g\":\"%s\",\"slave\":%u,\"online\":%u,\"pts\":[",
            gateway, (unsigned)slave_id, (unsigned)snap.is_online);
        if (n < 0 || n >= (int)sizeof(req.payload) - 2) return true;
        int included = 0;
        for (; first < npts; first++)
        {
            if (!pts[first].valid) continue;
            int w = snprintf(buf + n, sizeof(req.payload) - (size_t)n,
                "%s{\"n\":\"%s\",\"v\":%.2f,\"u\":\"%s\"}",
                included ? "," : "", pts[first].name, (double)pts[first].value, pts[first].unit);
            if (w < 0) return true;
            if (n + w >= (int)sizeof(req.payload) - 2) break;
            n += w;
            included++;
        }
        if (included == 0) return true;
        buf[n++] = ']';
        buf[n++] = '}';
        req.payload_len = (uint16_t)n;

        taskENTER_CRITICAL();
        bool allowed = !s_pause_requested && UpgradeQuiesce_AllowsMutation();
        osStatus_t st = allowed ? osMessageQueuePut(storage_m2_write_queue(), &req, 0, 0) : osErrorResource;
        taskEXIT_CRITICAL();
        if (!allowed) return true;
        if (st != osOK)
        {
            printf("[TEL] cache not accepted, seq=%08lX%08lX slave=%u status=%d\r\n",
                   (unsigned long)(seq >> 32U), (unsigned long)seq, (unsigned)slave_id, (int)st);
            return true;
        }
        // 入队不代表落盘；由MQTT拉取已提交记录后再发送。
        printf("[TEL] queued seq=%08lX%08lX slave=%u pts=%d bytes=%u\r\n",
               (unsigned long)(seq >> 32U), (unsigned long)seq,
               (unsigned)slave_id, included, (unsigned)req.payload_len);
    }
    return true;
}

/* 空 init：telemetry 没有自己的资源要初始化，全靠 M1/M3 已就绪 */
AppModuleInitResult Telemetry_Init(void)
{
    return APP_MODULE_INIT_OK;
}

/* 采集、合法空闲及暂停检查由本任务报告；超时不继续恢复健康状态。 */
void Telemetry_Task(void *argument)
{
    (void)argument;
    if (!Monitor_RegisterHealth(APP_MODULE_TELEMETRY, 2000U, 30000U))
        goto health_fail;
    /* 等系统初始化完（DHCP/MQTT/Modbus 都起来）再开始 */
    if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 6000U) ||
        osDelay(pdMS_TO_TICKS(5000U)) != osOK)
        goto health_fail;
    printf("[TEL] task started\r\n");
    uint32_t next_cycle_ms = HAL_GetTick();

    for (;;)
    {
        taskENTER_CRITICAL();
        bool healthy = Monitor_ReportProgress(APP_MODULE_TELEMETRY, true);
        const app_config_t *config = AppConfig_Get();
        bool paused = s_pause_requested || !UpgradeQuiesce_AllowsMutation() || config == NULL;
        s_pause_acknowledged = s_pause_requested && healthy;
        taskEXIT_CRITICAL();
        if (!healthy) goto health_fail;
        if (paused || (int32_t)(HAL_GetTick() - next_cycle_ms) < 0)
        {
            if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 1500U) ||
                osDelay(pdMS_TO_TICKS(500U)) != osOK)
                goto health_fail;
            continue;
        }

        /* 阶段1：收集有效从站 ID（只持 cfg 锁很短时间） */
        tel_slave_list_t list = {0};
        if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 3000U)) goto health_fail;
        uint8_t cnt = mb_for_each_valid_slave(collect_slave_ids, &list);
        if (!Monitor_ReportProgress(APP_MODULE_TELEMETRY, true)) goto health_fail;

        /* 阶段2：逐个从站处理（不持有任何 Modbus 锁） */
        for (uint8_t i = 0; i < list.n; i++)
        {
            if (s_pause_requested || !UpgradeQuiesce_AllowsMutation()) break;
            if (!publish_one_slave(list.ids[i])) goto health_fail;
        }

        printf("[TEL] cycle done: %u slaves\r\n", (unsigned)cnt);
        /* 长上报周期仍每500ms检查暂停和真实任务进展，不声明整周期的健康豁免。 */
        next_cycle_ms = HAL_GetTick() + config->mq_report_interval_ms;
        if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 1500U) ||
            osDelay(pdMS_TO_TICKS(500U)) != osOK)
            goto health_fail;
    }

health_fail:
    taskENTER_CRITICAL();
    s_pause_acknowledged = false;
    taskEXIT_CRITICAL();
    Monitor_ReportFatal(APP_MODULE_TELEMETRY);
    osThreadExit();
}
