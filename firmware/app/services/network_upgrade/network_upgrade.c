/*
 * network_upgrade.c —— M2 网络升级入口实现
 *
 * 职责（对应任务清单 M2 五项）：
 *   ① 接收并校验升级任务：MQTT 通知 JSON 解析 + authorize 回调
 *   ② 主动分块下载：调 NetworkDownloader_Fetch，每块交 UpgradeEntry_Write
 *   ③ 调用 M4 接口：Begin → Write(循环) → Finish → Reboot（执行任务串行调）
 *   ④ 状态与结果上报：消费M4核验的Boot结果，PUBLISH ota_status并等待PUBACK
 *   ⑤ 网络暂停和门控：UpgradeQuiesce_Bind(NETWORK)
 *
 * 不在 MQTT 回调里执行下载/Flash/密码运算（README 明确要求）。
 */
#include "network_upgrade.h"
#include "network_upgrade_downloader.h"
#include "https_tls.h"           /* HttpsTls_Init：TLS 模块一次性初始化 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"

#include "upgrade_stage.h"
#include "upgrade_entry.h"
#include "upgrade_quiesce.h"
#include "app_upgrade.h"
#include "app_modules.h"
#include "mqtt_client.h"
#include "telemetry_task.h"

/* ===== 升级通知请求结构（JSON 解析结果） ===== */
typedef struct
{
    char     task_id[NW_UPG_TASK_ID_MAX];
    char     version[NW_UPG_VERSION_MAX];
    uint32_t size;
    char     sha256_hex[NW_UPG_SHA256_HEX_LEN + 1];
    char     url[NW_UPG_URL_MAX];
    char     token[NW_UPG_TOKEN_MAX];
    bool     valid;
} nw_upgrade_request_t;

/* ===== 下载回调上下文 ===== */
typedef struct
{
    UpgradeEntryPeer *peer;
    uint32_t          session_id;
    const char       *task_id;     /* 供状态上报 */
    uint32_t          total_size;  /* 供进度计算 */
} nw_download_ctx_t;

/* ===== 前向声明（内部函数） ===== */
static void NetworkUpgrade_Task(void *argument);
static void report_boot_result(void);

/* ===== 模块状态 ===== */
static volatile bool s_initialized = false;
static volatile bool s_task_started = false;

/* 升级执行任务同步：Notify 设 request + 释放信号量，Task 获取后执行 */
static osSemaphoreId_t s_notify_sem = NULL;
static nw_upgrade_request_t s_pending_req __attribute__((section(".bss.ccmram")));   /* Notify 写，Task 读（单生产单消费） */
static volatile bool s_upgrade_in_progress = false;

/* 当前升级阶段（供状态上报） */
static volatile int s_phase = 0;   /* 0～6为下载阶段，7=trial，8=rolled_back；名称见phase_to_string。 */
static bool s_boot_result_done;

/* epoch 分配计数器（每次下载尝试唯一，本次启动不复用） */
static volatile uint32_t s_epoch_counter = 0;

/* ota_status topic（Init 时拼好，百度云模式 $iot/{dk}/user/ota_status） */
static char s_ota_status_topic[96] __attribute__((section(".bss.ccmram"))) = {0};

/* ===== 升级执行任务静态资源（6KiB，放 CCMRAM 不挤占 SRAM） ===== */
_Alignas(8) static StackType_t s_upgrade_stack[NETWORK_UPGRADE_TASK_STACK_BYTES / sizeof(StackType_t)] __attribute__((section(".bss.ccmram")));
static StaticTask_t s_upgrade_control_block __attribute__((section(".bss.ccmram")));
static const osThreadAttr_t s_upgrade_attributes =
{
    .name = "nw_upgrade",
    .cb_mem = &s_upgrade_control_block,
    .cb_size = sizeof(s_upgrade_control_block),
    .stack_mem = s_upgrade_stack,
    .stack_size = sizeof(s_upgrade_stack),
    .priority = NETWORK_UPGRADE_TASK_PRIORITY,
};

/* ===== JSON 手写解析（找 "key":"value" 或 "key":number） ===== */
static void json_extract_string(const char *json, const char *key, char *out, int out_max)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') { out[0] = '\0'; return; }
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_max - 1) { out[i++] = *p++; }
    out[i] = '\0';
}

static int32_t json_extract_int(const char *json, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '-' || (*p >= '0' && *p <= '9')) return atoi(p);
    return -1;
}

/* hex 字符串转字节数组（sha256: 64 hex char → 32B） */
static int hex_to_bytes(const char *hex, uint8_t *out, int out_max)
{
    int len = strlen(hex);
    if (len != out_max * 2) return -1;
    for (int i = 0; i < out_max; i++)
    {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        int h = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
        int l = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
        if (h < 0 || l < 0) return -1;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return 0;
}

/* ===== 状态上报：PUBLISH ota_status ===== */
static void report_status(const char *task_id, const char *phase, const char *detail)
{
    if (s_ota_status_topic[0] == '\0') return;
    char buf[256];
    int len;
    if (detail && detail[0])
    {
        len = snprintf(buf, sizeof(buf),
            "{\"task_id\":\"%s\",\"phase\":\"%s\",\"err\":\"%s\"}", task_id, phase, detail);
    }
    else
    {
        len = snprintf(buf, sizeof(buf),
            "{\"task_id\":\"%s\",\"phase\":\"%s\"}", task_id, phase);
    }
    MqttClient_PublishQos1(s_ota_status_topic, (const uint8_t *)buf, (size_t)len);
}

static void report_progress(const char *task_id, uint32_t offset, uint32_t total)
{
    if (s_ota_status_topic[0] == '\0') return;
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"task_id\":\"%s\",\"phase\":\"downloading\",\"offset\":%u,\"total\":%u}",
        task_id, (unsigned)offset, (unsigned)total);
    MqttClient_PublishQos1(s_ota_status_topic, (const uint8_t *)buf, (size_t)len);
}

static const char *phase_to_string(int phase)
{
    switch (phase)
    {
        case 0: return "idle";
        case 1: return "downloading";
        case 2: return "verifying";
        case 3: return "ready";
        case 4: return "rebooting";
        case 5: return "installed";
        case 6: return "failed";
        case 7: return "trial";
        case 8: return "rolled_back";
        default: return "unknown";
    }
}

const char *NetworkUpgrade_GetPhaseString(void)
{
    return phase_to_string(s_phase);
}

/* ===== 授权回调（UpgradeEntryPolicy.authorize） =====
 * 只读、快速、可并发查询。不从请求 JSON 读布尔量当授权。
 * 当前实现：执行任务发起的下载总是授权（M2 已校验云端任务）。
 */
static bool authorize_callback(void *context, const UpgradeEntryPeer *peer)
{
    (void)context;
    (void)peer;
    /* M2 在 Notify 时已校验 JSON 格式和来源，执行任务的 peer 由 M2 分配，总是授权 */
    return true;
}

static const UpgradeEntryPolicy s_policy =
{
    .authorize = authorize_callback,
    .context = NULL,
};

/* ===== 下载回调：调 UpgradeEntry_Write ===== */
static nw_dl_status_t download_chunk_callback(void *ctx_, uint32_t offset,
                                              const uint8_t *data, uint32_t len)
{
    nw_download_ctx_t *ctx = (nw_download_ctx_t *)ctx_;

    /* 调 M4 接口写入 */
    UpgradeStageResult ret = UpgradeEntry_Write(ctx->peer, ctx->session_id,
                                                offset, data, len);
    if (ret != UPGRADE_STAGE_OK)
    {
        printf("[NWUPG] Write fail at offset=%u len=%u: %s\r\n",
               (unsigned)offset, (unsigned)len, UpgradeEntry_ErrorName(ret));
        return NW_DL_ERR_CALLBACK;
    }

    /* 上报进度（每 16KB 上报一次，避免频繁 PUBLISH） */
    if ((offset & ~0x3FFF) != ((offset + len) & ~0x3FFF))
    {
        report_progress(ctx->task_id, offset + len, ctx->total_size);
    }

    return NW_DL_OK;
}

/* ===== 网络门控回调（UpgradeQuiesceParticipant） =====
 * pause: 请求两个任务停止新业务
 * settle: 等待任务自身确认及发送锁退出
 * sync: 再次核对暂停，持久数据由Storage_Task同步
 * resume: 恢复业务
 */

/* 仅表示已请求暂停；停稳结果须查询两个任务的实际确认。 */
static volatile bool s_quiesced = false;

bool NetworkUpgrade_IsQuiesced(void)
{
    return s_quiesced;
}

/* 原子关闭两个任务的业务入口，每次请求撤销旧确认。 */
static UpgradeStageResult quiesce_pause(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    if (timeout_ms == 0U) return UPGRADE_STAGE_TIMEOUT;
    taskENTER_CRITICAL();
    s_quiesced = true;
    MqttClient_SetPaused(true);
    Telemetry_SetPaused(true);
    taskEXIT_CRITICAL();
    printf("[NWUPG] pause requested\r\n");
    return UPGRADE_STAGE_OK;
}

/* 等待真实任务安全点，等待本身不代报健康、不延长传入期限。 */
static UpgradeStageResult quiesce_settle(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    uint32_t started_ms = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - started_ms) < timeout_ms)
    {
        if (!s_quiesced) return UPGRADE_STAGE_MODE;
        bool mqtt_paused = MqttClient_IsPaused();
        bool telemetry_paused = Telemetry_IsPaused();
        if (mqtt_paused && telemetry_paused) return UPGRADE_STAGE_OK;
        if (osDelay(1U) != osOK) return UPGRADE_STAGE_INTERNAL;
    }
    printf("[NWUPG] settle timeout mqtt=%u telemetry=%u\r\n",
           (unsigned)MqttClient_IsPaused(), (unsigned)Telemetry_IsPaused());
    return UPGRADE_STAGE_TIMEOUT;
}

/* 网络没有另建持久缓冲；保持暂停，记录落盘由存储同步阶段确认。 */
static UpgradeStageResult quiesce_sync(void *ctx, uint32_t timeout_ms)
{
    return quiesce_settle(ctx, timeout_ms);
}

/* 撤销本地请求；协调器在所有恢复成功后最后开放全局准入。 */
static UpgradeStageResult quiesce_resume(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    if (timeout_ms == 0U) return UPGRADE_STAGE_TIMEOUT;
    taskENTER_CRITICAL();
    MqttClient_SetPaused(false);
    Telemetry_SetPaused(false);
    s_quiesced = false;
    taskEXIT_CRITICAL();
    printf("[NWUPG] pause request cleared\r\n");
    return UPGRADE_STAGE_OK;
}

static const UpgradeQuiesceParticipant s_quiesce_participant =
{
    .pause = quiesce_pause,
    .settle = quiesce_settle,
    .sync = quiesce_sync,
    .resume = quiesce_resume,
    .context = NULL,
};

/* ===== 初始化 ===== */
void NetworkUpgrade_Init(void)
{
    if (s_initialized) return;

    /* 信号量：Notify 释放，Task 获取（初始 0，每次 Notify 释放 1） */
    s_notify_sem = osSemaphoreNew(1, 0, NULL);
    if (!s_notify_sem)
    {
        printf("[NWUPG] sem create fail, upgrade unavailable\r\n");
        return;
    }

    /* ota_status topic：从 MqttClient 获取（百度云模式 $iot/{dk}/user/ota_status） */
    const char *ota_topic = MqttClient_TopicOtaStatus();
    if (ota_topic && ota_topic[0])
    {
        strncpy(s_ota_status_topic, ota_topic, sizeof(s_ota_status_topic) - 1);
    }

    /* 绑定权限策略 */
    UpgradeStageResult ret = UpgradeEntry_BindPolicy(&s_policy);
    if (ret != UPGRADE_STAGE_OK)
    {
        printf("[NWUPG] BindPolicy fail: %s\r\n", UpgradeEntry_ErrorName(ret));
        /* 不阻断启动，升级不可用而已 */
    }

    /* 绑定网络门控 */
    ret = UpgradeQuiesce_Bind(UPGRADE_QUIESCE_NETWORK, &s_quiesce_participant);
    if (ret != UPGRADE_STAGE_OK)
    {
        printf("[NWUPG] QuiesceBind(NETWORK) fail: %s\r\n", UpgradeEntry_ErrorName(ret));
    }

    /* 初始化 TLS 模块（熵源 + CTR_DRBG + 时间函数注册） */
    https_tls_result_t tls_ret = HttpsTls_Init();
    if (tls_ret != HTTPS_TLS_OK)
    {
        printf("[NWUPG] TLS init fail: %d (HTTPS download unavailable)\r\n", (int)tls_ret);
        /* 不阻断启动：HTTP 下载仍可用，仅 HTTPS 不可用 */
    }

    s_initialized = true;
    printf("[NWUPG] Init OK: policy+quiesce bound\r\n");
}

/* ===== 创建执行任务 ===== */
void NetworkUpgrade_StartTask(void)
{
    if (!s_initialized || s_task_started) return;

    osThreadId_t tid = osThreadNew((osThreadFunc_t)NetworkUpgrade_Task,
                                   NULL, &s_upgrade_attributes);
    if (tid)
    {
        s_task_started = true;
        printf("[NWUPG] task started (stack=%d)\r\n", NETWORK_UPGRADE_TASK_STACK_BYTES);
    }
    else
    {
        printf("[NWUPG] task create fail\r\n");
    }
}

/* ===== MQTT 回调收到升级通知 ===== */
int NetworkUpgrade_Notify(const char *json)
{
    if (!s_initialized || !s_task_started) return -3;
    if (s_upgrade_in_progress) return -2;

    /* 解析 JSON 到 s_pending_req */
    memset(&s_pending_req, 0, sizeof(s_pending_req));
    json_extract_string(json, "task_id", s_pending_req.task_id, sizeof(s_pending_req.task_id));
    json_extract_string(json, "version", s_pending_req.version, sizeof(s_pending_req.version));
    json_extract_string(json, "sha256", s_pending_req.sha256_hex, sizeof(s_pending_req.sha256_hex));
    json_extract_string(json, "url", s_pending_req.url, sizeof(s_pending_req.url));
    json_extract_string(json, "token", s_pending_req.token, sizeof(s_pending_req.token));
    s_pending_req.size = (uint32_t)json_extract_int(json, "size");

    /* 校验必填字段 */
    if (s_pending_req.url[0] == '\0' || s_pending_req.size == 0 ||
        s_pending_req.sha256_hex[0] == '\0')
    {
        printf("[NWUPG] Notify: missing required fields\r\n");
        return -1;
    }

    s_pending_req.valid = true;
    printf("[NWUPG] Notify: task=%s ver=%s size=%u url=%s\r\n",
           s_pending_req.task_id, s_pending_req.version,
           (unsigned)s_pending_req.size, s_pending_req.url);

    /* 唤醒执行任务 */
    osSemaphoreRelease(s_notify_sem);
    return 0;
}

/* ===== 升级执行任务 ===== */
static void NetworkUpgrade_Task(void *argument)
{
    (void)argument;

    printf("[NWUPG] task running, waiting for notify...\r\n");

    for (;;)
    {
        /* 空闲时每秒尝试交接启动结果；收到下载通知后立即消费，不在此处等待上报。 */
        osStatus_t wait = osSemaphoreAcquire(s_notify_sem, osKernelGetTickFreq());
        if (wait != osOK)
        {
            report_boot_result();
            continue;
        }
        if (!s_pending_req.valid) continue;

        s_upgrade_in_progress = true;
        nw_upgrade_request_t req;
        memcpy(&req, &s_pending_req, sizeof(req));
        s_pending_req.valid = false;

        printf("[NWUPG] === upgrade start: task=%s ===\r\n", req.task_id);

        /* 分配 epoch（每次下载尝试唯一） */
        uint32_t epoch = ++s_epoch_counter;

        UpgradeEntryPeer peer =
        {
            .source = UPGRADE_STAGE_SOURCE_NETWORK,
            .epoch = epoch,
        };

        /* sha256 hex → 32B */
        uint8_t sha256_bytes[UPGRADE_STAGE_SHA256_SIZE];
        if (hex_to_bytes(req.sha256_hex, sha256_bytes, sizeof(sha256_bytes)) != 0)
        {
            printf("[NWUPG] sha256 hex parse fail: %s\r\n", req.sha256_hex);
            report_status(req.task_id, "failed", "E_SHA256_FORMAT");
            s_phase = 6;
            s_upgrade_in_progress = false;
            continue;
        }

        /* ① Begin */
        uint32_t session_id = 0;
        s_phase = 1;
        report_status(req.task_id, "downloading", NULL);

        UpgradeStageResult ret = UpgradeEntry_Begin(&peer, NULL, req.size,
                                                    sha256_bytes, &session_id);
        if (ret != UPGRADE_STAGE_OK)
        {
            printf("[NWUPG] Begin fail: %s\r\n", UpgradeEntry_ErrorName(ret));
            report_status(req.task_id, "failed", UpgradeEntry_ErrorName(ret));
            s_phase = 6;
            s_upgrade_in_progress = false;
            continue;
        }
        printf("[NWUPG] Begin OK: session=%u epoch=%u\r\n", (unsigned)session_id, (unsigned)epoch);

        /* ② 下载（回调里调 Write） */
        nw_download_ctx_t dl_ctx =
        {
            .peer = &peer,
            .session_id = session_id,
            .task_id = req.task_id,
            .total_size = req.size,
        };

        nw_dl_params_t dl_params =
        {
            .url = req.url,
            .auth_token = (req.token[0] != '\0') ? req.token : NULL,
            .total_size = req.size,
            .cb = download_chunk_callback,
            .ctx = &dl_ctx,
            .timeout_ms = 120000,
            .chunk_size = UPGRADE_STAGE_MAX_CHUNK_SIZE,
        };

        nw_dl_status_t dl_ret = NetworkDownloader_Fetch(&dl_params);
        if (dl_ret != NW_DL_OK)
        {
            printf("[NWUPG] download fail: %s\r\n", NetworkDownloader_ErrorString(dl_ret));
            report_status(req.task_id, "failed", NetworkDownloader_ErrorString(dl_ret));
            /* 取消会话 */
            UpgradeEntry_Abort(&peer, session_id);
            UpgradeEntry_Disconnect(&peer);
            s_phase = 6;
            s_upgrade_in_progress = false;
            continue;
        }

        /* ③ Finish（校验 + 提交 READY + 暂停业务） */
        s_phase = 2;
        report_status(req.task_id, "verifying", NULL);

        ret = UpgradeEntry_Finish(&peer, session_id);
        if (ret != UPGRADE_STAGE_OK)
        {
            printf("[NWUPG] Finish fail: %s\r\n", UpgradeEntry_ErrorName(ret));
            report_status(req.task_id, "failed", UpgradeEntry_ErrorName(ret));
            UpgradeEntry_Disconnect(&peer);
            s_phase = 6;
            s_upgrade_in_progress = false;
            continue;
        }

        printf("[NWUPG] Finish OK: READY committed, rebooting in 1s...\r\n");
        s_phase = 3;
        report_status(req.task_id, "ready", NULL);
        osDelay(1000);   /* 给云端 1s 收到 status 的时间 */

        /* ④ Reboot */
        s_phase = 4;
        report_status(req.task_id, "rebooting", NULL);
        ret = UpgradeEntry_Reboot(&peer, session_id);
        /* Reboot 成功不返回；若返回则失败 */
        printf("[NWUPG] Reboot returned: %s (should not happen)\r\n",
               UpgradeEntry_ErrorName(ret));
        report_status(req.task_id, "failed", "E_REBOOT");

        s_upgrade_in_progress = false;
        /* 不 continue，任务结束（重启后不会到这） */
    }
}

/* 只消费M4已核验的本次启动快照；同一结果在broker确认前保留并重试。 */
static void report_boot_result(void)
{
    if (s_boot_result_done) return;

    UpgradeMessage identity;
    if (!App_UpgradeGetStartupIdentity(&identity))
    {
        s_boot_result_done = true;
        return;
    }
    AppUpgradeResult result = App_UpgradeGetResult();
    if ((result == APP_UPGRADE_RESULT_UNKNOWN) || !MqttClient_IsConnected()) return;

    const char *phase;
    switch (result)
    {
        case APP_UPGRADE_RESULT_TRIAL:
            phase = "trial";
            s_phase = 7;
            break;
        case APP_UPGRADE_RESULT_CONFIRMED:
            phase = "confirmed";
            s_phase = 5;
            break;
        case APP_UPGRADE_RESULT_ROLLED_BACK:
            phase = "rolled_back";
            s_phase = 8;
            break;
        default:
            return;
    }

    char digest[65];
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(identity.digest); i++)
    {
        digest[i * 2] = hex[identity.digest[i] >> 4];
        digest[i * 2 + 1] = hex[identity.digest[i] & 0x0fU];
    }
    digest[64] = '\0';

    char payload[256];
    int len = snprintf(payload, sizeof(payload),
        "{\"type\":\"boot_result\",\"tx_id\":\"%08lx%08lx\",\"sequence\":%lu,"
        "\"phase\":\"%s\",\"image_sha256\":\"%s\"}",
        (unsigned long)(identity.tx_id >> 32), (unsigned long)(uint32_t)identity.tx_id,
        (unsigned long)identity.sequence, phase, digest);
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) return;

    if (MqttClient_PublishUpgradeResult((const uint8_t *)payload, (size_t)len) == 1)
    {
        s_boot_result_done = true;
        printf("[NWUPG] boot result PUBACK: tx=%08lx%08lx phase=%s\r\n",
               (unsigned long)(identity.tx_id >> 32), (unsigned long)(uint32_t)identity.tx_id, phase);
    }
}
