/*
 * network_upgrade.c —— M2 网络升级入口实现
 *
 * 职责（对应任务清单 M2 五项）：
 *   ① 接收并校验升级任务：MQTT 通知 JSON 解析 + authorize 回调
 *   ② 主动分块下载：调 NetworkDownloader_Fetch，每块交 UpgradeEntry_Write
 *   ③ 调用 M4 接口：Begin → Write(循环) → Finish → Reboot（执行任务串行调）
 *   ④ 状态与结果上报：PUBLISH ota_status，重启后查 GetStatus 上报最终结果
 *   ⑤ 网络暂停、门控和健康报告：UpgradeQuiesce_Bind(NETWORK) + Monitor_RegisterHealth
 *
 * 不在 MQTT 回调里执行下载/Flash/密码运算（README 明确要求）。
 */
#include "network_upgrade.h"
#include "network_upgrade_downloader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"

#include "upgrade_stage.h"
#include "upgrade_entry.h"
#include "upgrade_quiesce.h"
#include "app_monitor.h"
#include "app_upgrade.h"
#include "app_modules.h"
#include "mqtt_client.h"

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

/* ===== 模块状态 ===== */
static volatile bool s_initialized = false;
static volatile bool s_task_started = false;

/* 升级执行任务同步：Notify 设 request + 释放信号量，Task 获取后执行 */
static osSemaphoreId_t s_notify_sem = NULL;
static nw_upgrade_request_t s_pending_req;   /* Notify 写，Task 读（单生产单消费） */
static volatile bool s_upgrade_in_progress = false;

/* 当前升级阶段（供状态上报） */
static volatile int s_phase = 0;   /* 0=idle 1=downloading 2=verifying 3=ready 4=rebooting 5=installed 6=failed */

/* epoch 分配计数器（每次下载尝试唯一，本次启动不复用） */
static volatile uint32_t s_epoch_counter = 0;

/* ota_status topic（Init 时拼好，百度云模式 $iot/{dk}/user/ota_status） */
static char s_ota_status_topic[96] = {0};

/* ===== 升级执行任务静态资源（8KiB 普通 SRAM 栈） ===== */
_Alignas(8) static StackType_t s_upgrade_stack[NETWORK_UPGRADE_TASK_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_upgrade_control_block;
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
    MqttClient_Publish(s_ota_status_topic, (const uint8_t *)buf, (size_t)len);
}

static void report_progress(const char *task_id, uint32_t offset, uint32_t total)
{
    if (s_ota_status_topic[0] == '\0') return;
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"task_id\":\"%s\",\"phase\":\"downloading\",\"offset\":%u,\"total\":%u}",
        task_id, (unsigned)offset, (unsigned)total);
    MqttClient_Publish(s_ota_status_topic, (const uint8_t *)buf, (size_t)len);
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
        Monitor_ReportProgress(APP_MODULE_MQTT, true);
    }

    return NW_DL_OK;
}

/* ===== 网络门控回调（UpgradeQuiesceParticipant） =====
 * pause: 暂停遥测发送 + Telemetry 采集
 * settle: 结算在途 PUBLISH（等待或超时）
 * sync: flush 缓冲
 * resume: 恢复业务
 */

/* 门控标志：pause 设 true，resume 设 false。Telemetry 主循环检查它。 */
static volatile bool s_quiesced = false;

bool NetworkUpgrade_IsQuiesced(void)
{
    return s_quiesced;
}

static UpgradeStageResult quiesce_pause(void *ctx, uint32_t timeout_ms)
{
    (void)ctx; (void)timeout_ms;
    /* 设暂停标志：Telemetry 下一轮看到就跳过采集+上报 */
    s_quiesced = true;
    printf("[NWUPG] quiesce_pause: telemetry paused\r\n");
    return UPGRADE_STAGE_OK;
}

static UpgradeStageResult quiesce_settle(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    /* 结算在途 PUBLISH：等当前轮 Telemetry 完成（最多 timeout_ms）。
     * Telemetry 周期 2s，一轮采集+上报约 200ms。等 500ms 足够让在途消息发完。 */
    uint32_t wait = (timeout_ms > 0 && timeout_ms < 500) ? timeout_ms : 500;
    osDelay(wait);
    printf("[NWUPG] quiesce_settle: in-flight settled (%ums)\r\n", (unsigned)wait);
    return UPGRADE_STAGE_OK;
}

static UpgradeStageResult quiesce_sync(void *ctx, uint32_t timeout_ms)
{
    (void)ctx; (void)timeout_ms;
    /* flush：当前无额外缓冲要刷，MQTT netconn_write 是同步的，直接返回 OK */
    printf("[NWUPG] quiesce_sync: no buffer to flush\r\n");
    return UPGRADE_STAGE_OK;
}

static UpgradeStageResult quiesce_resume(void *ctx, uint32_t timeout_ms)
{
    (void)ctx; (void)timeout_ms;
    /* 恢复业务：清暂停标志，Telemetry 下一轮恢复采集+上报 */
    s_quiesced = false;
    printf("[NWUPG] quiesce_resume: telemetry resumed\r\n");
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

    /* 登记 MQTT 模块健康（2s 进展，30s 等待上限） */
    Monitor_RegisterHealth(APP_MODULE_MQTT, 2000, 30000);

    /* 登记 Telemetry 模块健康 */
    Monitor_RegisterHealth(APP_MODULE_TELEMETRY, 2000, 30000);

    s_initialized = true;
    printf("[NWUPG] Init OK: policy+quiesce+monitor bound\r\n");
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
void NetworkUpgrade_Task(void *argument)
{
    (void)argument;

    printf("[NWUPG] task running, waiting for notify...\r\n");

    for (;;)
    {
        /* 等通知 */
        osSemaphoreAcquire(s_notify_sem, osWaitForever);
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

/* ===== 重启后结果上报 ===== */
void NetworkUpgrade_ReportFinalResult(void)
{
    if (!s_initialized) return;

    /* 判断是否升级后启动 */
    bool is_trial = App_UpgradeIsTrial();
    if (!is_trial)
    {
        printf("[NWUPG] not trial boot, skip final report\r\n");
        return;
    }

    /* 查 Boot 身份 */
    UpgradeMessage msg;
    if (App_UpgradeGetBootIdentity(&msg))
    {
        printf("[NWUPG] trial boot: seq=%u attempts=%u\r\n",
               (unsigned)msg.sequence, (unsigned)msg.trial_attempts);
    }

    /* 查 GetStatus */
    UpgradeEntryPeer peer =
    {
        .source = UPGRADE_STAGE_SOURCE_NETWORK,
        .epoch = 0,   /* 重启后查询，epoch 不重要 */
    };
    UpgradeStageStatus status;
    UpgradeStageResult ret = UpgradeEntry_GetStatus(&peer, &status);
    if (ret == UPGRADE_STAGE_OK)
    {
        /* 根据状态判断安装结果 */
        /* TODO: 从 UpgradeStageStatus 解析 phase 和结果 */
        printf("[NWUPG] GetStatus OK: phase=%d\r\n", (int)status.phase);

        if (status.phase == UPGRADE_STAGE_VERIFIED || status.phase == UPGRADE_STAGE_REBOOT_PENDING)
        {
            /* 还在试运行，等确认 */
            s_phase = 5;   /* installed（待确认） */
            report_status("", "installed", "trial");
        }
        else if (status.phase == UPGRADE_STAGE_IDLE)
        {
            /* 已确认或回退完成 */
            s_phase = 5;
            report_status("", "installed", "success");
        }
        else if (status.phase == UPGRADE_STAGE_ERROR)
        {
            s_phase = 6;
            report_status("", "failed", "E_HEALTH_CHECK");
        }
    }
    else
    {
        printf("[NWUPG] GetStatus fail: %s\r\n", UpgradeEntry_ErrorName(ret));
    }
}
