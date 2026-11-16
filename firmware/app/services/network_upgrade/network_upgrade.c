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
 * MQTT只解析并散列通知；ECDSA、RTC和下载在原网络升级任务中执行。
 */
#include "network_upgrade.h"
#include "network_upgrade_downloader.h"
#include "https_tls.h"           /* HttpsTls_Init：TLS 模块一次性初始化 */

#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"

#include "upgrade_stage.h"
#include "upgrade_entry.h"
#include "upgrade_quiesce.h"
#include "app_upgrade.h"
#include "app_modules.h"
#include "app_monitor.h"
#include "mqtt_client.h"
#include "telemetry_task.h"
#include "storage_time.h"
#include "storage_identity.h"
#include "mbedtls/sha256.h"

/* ===== 升级通知请求结构（JSON 解析结果） ===== */
typedef struct
{
    char     task_id[NW_UPG_TASK_ID_MAX];
    char     version[NW_UPG_VERSION_MAX];
    uint32_t size;
    uint8_t  sha256[UPGRADE_STAGE_SHA256_SIZE];
    char     url[NW_UPG_URL_MAX];
    char     token[NW_UPG_TOKEN_MAX];
    uint32_t issued_at;
    uint32_t expires_at;
    uint8_t  notice_digest[32];
    uint8_t  signature[64];
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
static void report_boot_result(void);

/* ===== 模块状态 ===== */
static volatile bool s_initialized = false;
static volatile bool s_task_started = false;
static osThreadId_t s_upgrade_thread;
static NetworkUpgradeAuthorization s_authorization;
static bool s_authorization_bound;
static bool s_authorized;
static bool s_tls_ready;
static bool s_revoked;
static uint32_t s_authorized_until_ms;
static uint32_t s_last_issued_at;
static bool s_cleanup_pending; // 仅执行任务读写，区别于Notify尚未发布的请求。

/* 升级执行任务同步：Notify 设 request + 释放信号量，Task 获取后执行 */
static osSemaphoreId_t s_notify_sem = NULL;
static nw_upgrade_request_t s_pending_req __attribute__((section(".bss.ccmram")));   /* Notify 写，Task 读（单生产单消费） */
static volatile bool s_upgrade_in_progress = false;

/* 当前升级阶段（供状态上报） */
static volatile int s_phase = 0;   /* 0～6为下载阶段，7=trial，8=rolled_back；名称见phase_to_string。 */
static bool s_boot_result_done;

/* epoch 分配计数器（每次下载尝试唯一，本次启动不复用） */
static volatile uint32_t s_epoch_counter = 0;

// 按工具生成的固定字段顺序读ASCII字符串；无空白、转义、截断或未知字段。
static bool notice_text(const char **cursor, const char *key, char *out, size_t capacity)
{
    size_t n = strlen(key);
    if (strncmp(*cursor, key, n) != 0) return false;
    const char *p = *cursor + n;
    size_t length = 0U;
    while (*p != '"')
    {
        if ((unsigned char)*p < 0x20U || (unsigned char)*p >= 0x7FU || *p == '\\' ||
            length + 1U >= capacity) return false;
        out[length++] = *p++;
    }
    out[length] = '\0';
    *cursor = p + 1;
    return true;
}

// 接受规范十进制uint32，不允许符号、前导零、小数或累加溢出。
static bool notice_uint(const char **cursor, const char *key, uint32_t *out)
{
    size_t n = strlen(key);
    if (strncmp(*cursor, key, n) != 0) return false;
    const char *p = *cursor + n;
    if (*p < '0' || *p > '9' || (*p == '0' && p[1] >= '0' && p[1] <= '9')) return false;
    uint32_t value = 0U;
    while (*p >= '0' && *p <= '9')
    {
        uint32_t digit = (uint32_t)(*p++ - '0');
        if (value > (UINT32_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    *cursor = p;
    *out = value;
    return true;
}

/* hex 字符串转字节数组（sha256: 64 hex char → 32B）
 * 返回 0=成功，-1=长度不符或含非法字符
 * 用于把通知 JSON 里的 sha256 hex 字符串转成 32 字节摘要 */
static int hex_to_bytes(const char *hex, uint8_t *out, int out_max)
{
    int len = strlen(hex);
    if (len != out_max * 2) return -1;   /* hex 字符数必须 = 字节数×2 */
    for (int i = 0; i < out_max; i++)
    {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        /* hex 字符转数值：0-9/a-f/A-F */
        int h = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
        int l = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
        if (h < 0 || l < 0) return -1;
        out[i] = (uint8_t)((h << 4) | l);   /* 高 4 位 + 低 4 位拼一个字节 */
    }
    return 0;
}

// 严格解析并散列签名覆盖的原始正文；MQTT任务只做有界SHA256，不执行ECDSA。
static bool parse_notice(const char *json, nw_upgrade_request_t *request)
{
    static const char start[] = "{\"type\":\"upgrade\"";
    static const char domain[] = "GWOTA-NOTICE-V1\n";
    if (strlen(json) > NW_UPG_NOTICE_MAX || strncmp(json, start, sizeof(start) - 1U) != 0) return false;
    const char *p = json + sizeof(start) - 1U;
    char text[129], gateway[GATEWAY_ID_MAX_LEN];
    UpgradeMessage current;
    if (!notice_text(&p, ",\"task_id\":\"", request->task_id, sizeof(request->task_id)) ||
        !notice_text(&p, ",\"gateway_id\":\"", text, GATEWAY_ID_MAX_LEN) ||
        !MqttClient_GetGatewayId(gateway, sizeof(gateway)) || strcmp(text, gateway) != 0 ||
        !notice_text(&p, ",\"version\":\"", request->version, sizeof(request->version)) ||
        !notice_uint(&p, ",\"size\":", &request->size) ||
        !notice_text(&p, ",\"sha256\":\"", text, 65U) ||
        hex_to_bytes(text, request->sha256, 32) != 0 ||
        !notice_text(&p, ",\"image_sha256\":\"", text, 65U) ||
        hex_to_bytes(text, request->notice_digest, 32) != 0 ||
        !App_UpgradeGetStartupIdentity(&current) ||
        memcmp(current.digest, request->notice_digest, 32U) == 0 ||
        !notice_text(&p, ",\"url\":\"", request->url, sizeof(request->url)) ||
        !notice_text(&p, ",\"token\":\"", request->token, sizeof(request->token)) ||
        !notice_uint(&p, ",\"issued_at\":", &request->issued_at) ||
        !notice_uint(&p, ",\"expires_at\":", &request->expires_at)) return false;
    const char *signature_key = p;
    if (!notice_text(&p, ",\"signature\":\"", text, sizeof(text)) || strcmp(p, "}") != 0 ||
        hex_to_bytes(text, request->signature, 64) != 0 || request->task_id[0] == '\0' ||
        request->version[0] == '\0' || request->size < UPGRADE_STAGE_MIN_PACKAGE_SIZE ||
        request->size > UPGRADE_STAGE_MAX_PACKAGE_SIZE || request->expires_at <= request->issued_at ||
        request->expires_at - request->issued_at > NW_UPG_NOTICE_MAX_VALIDITY_S ||
        strncmp(request->url, NW_UPG_BOS_PREFIX, sizeof(NW_UPG_BOS_PREFIX) - 1U) != 0 ||
        request->url[sizeof(NW_UPG_BOS_PREFIX) - 1U] == '\0' || strpbrk(request->url, " #") != NULL)
        return false;
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    int result = mbedtls_sha256_starts(&hash, 0);
    if (result == 0) result = mbedtls_sha256_update(&hash, (const uint8_t *)domain, sizeof(domain) - 1U);
    if (result == 0) result = mbedtls_sha256_update(&hash, (const uint8_t *)json, (size_t)(signature_key - json));
    if (result == 0) result = mbedtls_sha256_update(&hash, (const uint8_t *)"}", 1U);
    if (result == 0) result = mbedtls_sha256_finish(&hash, request->notice_digest);
    mbedtls_sha256_free(&hash);
    return result == 0;
}

// 通知验签在原网络升级任务执行，Stage操作锁与包认证共用。
static UpgradeStageResult verify_notice(void *context, const uint8_t *digest, const uint8_t *signature)
{
    (void)context;
    return UpgradeStage_VerifyNotice(digest, signature);
}

// UTC秒须在签名窗口内，且晚于本次启动已核验通知的签发时间。
static bool notice_time_valid(const nw_upgrade_request_t *request, uint32_t utc)
{
    return utc != 0U && request->issued_at <= utc && request->expires_at > utc
        && request->issued_at > s_last_issued_at;
}

/* ===== 状态上报：PUBLISH ota_status ===== */
static void report_status(const char *task_id, const char *phase, const char *detail)
{
    const char *topic = MqttClient_TopicOtaStatus();
    if (!topic || topic[0] == '\0') return;
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
    MqttClient_PublishQos1(topic, (const uint8_t *)buf, (size_t)len);
}

/* 上报下载进度（偏移量 + 总大小）
 * JSON: {"task_id":"...","phase":"downloading","offset":4096,"total":262144}
 * 每 16KB 上报一次，避免频繁 PUBLISH 占带宽 */
static void report_progress(const char *task_id, uint32_t offset, uint32_t total)
{
    const char *topic = MqttClient_TopicOtaStatus();
    if (!topic || topic[0] == '\0') return;
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"task_id\":\"%s\",\"phase\":\"downloading\",\"offset\":%u,\"total\":%u}",
        task_id, (unsigned)offset, (unsigned)total);
    MqttClient_PublishQos1(topic, (const uint8_t *)buf, (size_t)len);
}

/* 阶段编号 → 可读字符串（供云端和日志显示） */
static const char *phase_to_string(int phase)
{
    switch (phase)
    {
        case 0: return "idle";         /* 空闲 */
        case 1: return "downloading";  /* 下载中 */
        case 2: return "verifying";    /* 校验中（SHA256+签名） */
        case 3: return "ready";        /* 校验通过，准备重启 */
        case 4: return "rebooting";    /* 正在重启 */
        case 5: return "installed";    /* 安装确认（Boot 核验通过） */
        case 6: return "failed";       /* 失败 */
        case 7: return "trial";        /* 试运行（Boot 核验通过但未确认） */
        case 8: return "rolled_back";  /* 回退到已确认的旧镜像 */
        default: return "unknown";
    }
}

const char *NetworkUpgrade_GetPhaseString(void)
{
    return phase_to_string(s_phase);
}

// 在初始化前绑定完整通知的核验回调，不将格式正确当作授权。
UpgradeStageResult NetworkUpgrade_BindAuthorization(const NetworkUpgradeAuthorization *authorization)
{
    if (!authorization || !authorization->verify) return UPGRADE_STAGE_INVALID;
    if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning)) return UPGRADE_STAGE_MODE;
    taskENTER_CRITICAL();
    if (s_authorization_bound || s_initialized)
    {
        taskEXIT_CRITICAL();
        return UPGRADE_STAGE_BUSY;
    }
    s_authorization = *authorization;
    s_authorization_bound = true;
    taskEXIT_CRITICAL();
    return UPGRADE_STAGE_OK;
}

// 只允许网络执行任务使用当前已核验通知对应的epoch及尚未撤销的许可。
bool NetworkUpgrade_Authorize(const UpgradeEntryPeer *peer)
{
    if (!peer || (peer->source != UPGRADE_STAGE_SOURCE_NETWORK) || (peer->epoch == 0U)
        || (__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning)
        || (osThreadGetId() != s_upgrade_thread)) return false;
    taskENTER_CRITICAL();
    bool allowed = s_initialized && s_task_started && s_authorization_bound
                && s_upgrade_in_progress && s_authorized && !s_revoked
                && (int32_t)(s_authorized_until_ms - HAL_GetTick()) > 0
                && (peer->epoch == s_epoch_counter);
    taskEXIT_CRITICAL();
    return allowed;
}

// MQTT任务撤销当前通知许可；即使认证正在执行，也用原epoch请求Stage取消。
UpgradeStageResult NetworkUpgrade_Revoke(void)
{
    if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning)
        || !App_IsCurrentModuleTask(APP_MODULE_MQTT)) return UPGRADE_STAGE_MODE;
    taskENTER_CRITICAL();
    bool active = s_upgrade_in_progress;
    s_revoked = true;
    UpgradeEntryPeer peer = {UPGRADE_STAGE_SOURCE_NETWORK, s_epoch_counter};
    s_authorized = false;
    taskEXIT_CRITICAL();
    return active ? UpgradeEntry_Disconnect(&peer) : UPGRADE_STAGE_MODE;
}

// 取消并结算原会话；失败或提交结果不明时保留占用，由执行任务空闲轮询重试。
static void cleanup_attempt(const UpgradeEntryPeer *peer)
{
    taskENTER_CRITICAL();
    s_authorized = false;
    taskEXIT_CRITICAL();
    UpgradeStageResult result = UpgradeEntry_Disconnect(peer);
    UpgradeStageStatus status;
    bool closed = result == UPGRADE_STAGE_OK;
    if (result == UPGRADE_STAGE_MODE)
    {
        closed = (UpgradeStage_GetStatus(&status) == UPGRADE_STAGE_OK)
              && !status.busy && !status.cleanup_pending && (status.resume_error == UPGRADE_STAGE_OK)
              && ((status.phase == UPGRADE_STAGE_IDLE) || (status.phase == UPGRADE_STAGE_ERROR));
    }
    if (closed)
    {
        taskENTER_CRITICAL();
        s_upgrade_in_progress = false;
        taskEXIT_CRITICAL();
    }
    else if (!s_cleanup_pending)
    {
        printf("[NWUPG] cleanup pending: epoch=%u result=%s\r\n",
               (unsigned)peer->epoch, UpgradeEntry_ErrorName(result));
    }
    s_cleanup_pending = !closed;
}

/* ===== 下载回调：调 UpgradeEntry_Write =====
 * 由 NetworkDownloader_Fetch 每收到一块数据时回调。
 * offset = 当前块在整个文件中的字节偏移
 * data/len = 当前块数据（≤1016B）
 * 回调里调 M4 的 UpgradeEntry_Write 把数据写入 Flash。 */
static nw_dl_status_t download_chunk_callback(void *ctx_, uint32_t offset,
                                              const uint8_t *data, uint32_t len)
{
    nw_download_ctx_t *ctx = (nw_download_ctx_t *)ctx_;

    /* 调 M4 接口写入 Flash（UpgradeEntry_Write 内部处理 Flash 写入 + 偏移校验） */
    UpgradeStageResult ret = UpgradeEntry_Write(ctx->peer, ctx->session_id,
                                                offset, data, len);
    if (ret != UPGRADE_STAGE_OK)
    {
        printf("[NWUPG] Write fail at offset=%u len=%u: %s\r\n",
               (unsigned)offset, (unsigned)len, UpgradeEntry_ErrorName(ret));
        return NW_DL_ERR_CALLBACK;
    }

    /* 上报进度（每 16KB 上报一次，用位运算判断是否跨越 16KB 边界） */
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
AppModuleInitResult NetworkUpgrade_Init(void)
{
    if (s_initialized) return APP_MODULE_INIT_OK;
    if (!s_authorization_bound)
    {
        const NetworkUpgradeAuthorization authorization = {verify_notice, NULL};
        if (NetworkUpgrade_BindAuthorization(&authorization) != UPGRADE_STAGE_OK)
            return APP_MODULE_INIT_FAILED;
    }

    /* 信号量：Notify 释放，Task 获取（初始 0，每次 Notify 释放 1） */
    if (!s_notify_sem) s_notify_sem = osSemaphoreNew(1, 0, NULL);
    if (!s_notify_sem)
    {
        printf("[NWUPG] sem create fail, upgrade unavailable\r\n");
        return APP_MODULE_INIT_FAILED;
    }

    /* 绑定网络门控 */
    UpgradeStageResult ret = UpgradeQuiesce_Bind(UPGRADE_QUIESCE_NETWORK, &s_quiesce_participant);
    if (ret != UPGRADE_STAGE_OK)
    {
        printf("[NWUPG] QuiesceBind(NETWORK) fail: %s\r\n", UpgradeEntry_ErrorName(ret));
        if (osSemaphoreDelete(s_notify_sem) == osOK) s_notify_sem = NULL;
        return APP_MODULE_INIT_FAILED;
    }

    /* 初始化 TLS 模块（熵源 + CTR_DRBG + 时间函数注册） */
    https_tls_result_t tls_ret = HttpsTls_Init();
    s_tls_ready = tls_ret == HTTPS_TLS_OK;
    if (tls_ret != HTTPS_TLS_OK)
    {
        printf("[NWUPG] TLS init fail: %d (HTTPS download unavailable)\r\n", (int)tls_ret);
        // 保留结果上报，拒绝新的HTTPS升级任务，不回退到明文HTTP。

    }

    s_initialized = true;
    printf("[NWUPG] resources ready; task authorization %s\r\n",
           s_authorization_bound ? "bound" : "NOT_READY");
    return tls_ret == HTTPS_TLS_OK ? APP_MODULE_INIT_OK : APP_MODULE_INIT_NOT_READY;
}

// MQTT任务解析通知并计算摘要；返回0仅表示排队受理，签名与UTC许可由升级任务核验。
int NetworkUpgrade_Notify(const char *json)
{
    if (!json) return -1;
    if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning)
        || !App_IsCurrentModuleTask(APP_MODULE_MQTT)) return -3;
    taskENTER_CRITICAL();
    if (!s_initialized || !s_task_started || !s_authorization_bound || !s_tls_ready ||
        (s_epoch_counter == UINT32_MAX))
    {
        taskEXIT_CRITICAL();
        return -3;
    }
    if (s_upgrade_in_progress)
    {
        taskEXIT_CRITICAL();
        return -2;
    }
    // 发布请求前占用原单槽，防止执行任务取走前被下一条通知覆盖。
    s_upgrade_in_progress = true;
    s_authorized = false;
    s_revoked = false;
    ++s_epoch_counter;
    taskEXIT_CRITICAL();
    int result = -1;
    memset(&s_pending_req, 0, sizeof(s_pending_req));
    if (!parse_notice(json, &s_pending_req)) goto rejected;
    /* 这里只受理待验通知；执行任务验签通过前不发布升级许可。 */
    s_pending_req.valid = true;

    /* 唤醒执行任务 */
    if (osSemaphoreRelease(s_notify_sem) != osOK) goto rejected;
    return 0;

rejected:
    taskENTER_CRITICAL();
    s_pending_req.valid = false;
    s_authorized = false;
    s_upgrade_in_progress = false;
    taskEXIT_CRITICAL();
    return result;
}

/* 在原网络升级任务中登记监督，串行执行下载、认证与启动结果上报。 */
void NetworkUpgrade_Task(void *argument)
{
    (void)argument;

    if (!s_initialized ||
        !Monitor_RegisterHealth(APP_MODULE_NETWORK_UPGRADE, 3000U,
                                UPGRADE_STAGE_PREPARE_TIMEOUT_MS + 1000U))
    {
        Monitor_ReportFatal(APP_MODULE_NETWORK_UPGRADE);
        return;
    }
    taskENTER_CRITICAL();
    s_upgrade_thread = osThreadGetId();
    s_task_started = true;
    taskEXIT_CRITICAL();

    printf("[NWUPG] task running, waiting for notify...\r\n");

    for (;;)
    {
        if (!Monitor_UpgradeProgress() || !Monitor_UpgradeWait(1000U))
        {
            Monitor_ReportFatal(APP_MODULE_NETWORK_UPGRADE);
            return;
        }
        /* 空闲时每秒尝试交接启动结果；收到下载通知后立即消费，不在此处等待上报。 */
        osStatus_t wait = osSemaphoreAcquire(s_notify_sem, osKernelGetTickFreq());
        if (!Monitor_UpgradeProgress())
        {
            Monitor_ReportFatal(APP_MODULE_NETWORK_UPGRADE);
            return;
        }
        if (wait != osOK)
        {
            if (s_cleanup_pending)
            {
                UpgradeEntryPeer previous = {UPGRADE_STAGE_SOURCE_NETWORK, s_epoch_counter};
                cleanup_attempt(&previous);
            }
            report_boot_result();
            continue;
        }
        if (!s_pending_req.valid) continue;

        nw_upgrade_request_t req;
        memcpy(&req, &s_pending_req, sizeof(req));
        s_pending_req.valid = false;

        if (!Monitor_UpgradeWait(3000U)) return;
        uint32_t utc = storage_get_unix_time();
        if (!Monitor_UpgradeProgress()) return;
        UpgradeStageResult authorized = UPGRADE_STAGE_AUTH;
        if (notice_time_valid(&req, utc))
            authorized = s_authorization.verify(s_authorization.context, req.notice_digest, req.signature);
        if (authorized == UPGRADE_STAGE_OK)
        {
            if (!Monitor_UpgradeWait(3000U)) return;
            utc = storage_get_unix_time();
            if (!Monitor_UpgradeProgress()) return;
            if (!notice_time_valid(&req, utc)) authorized = UPGRADE_STAGE_AUTH;
        }
        taskENTER_CRITICAL();
        if (authorized == UPGRADE_STAGE_OK)
        {
            s_last_issued_at = req.issued_at;
            if (s_revoked) authorized = UPGRADE_STAGE_MODE;
            else
            {
                s_authorized_until_ms = HAL_GetTick() + (req.expires_at - utc) * 1000U;
                s_authorized = true;
            }
        }
        if (authorized != UPGRADE_STAGE_OK) s_upgrade_in_progress = false;
        taskEXIT_CRITICAL();
        if (authorized != UPGRADE_STAGE_OK)
        {
            s_phase = 6;
            report_status(req.task_id, "failed", UpgradeEntry_ErrorName(authorized));
            continue;
        }

        printf("[NWUPG] === upgrade start: task=%s ===\r\n", req.task_id);
        AppMemory_Report("network-begin");

        /* 使用Notify准入时分配的epoch，取消结算前不允许下一条通知替换。 */
        uint32_t epoch = s_epoch_counter;

        UpgradeEntryPeer peer =
        {
            .source = UPGRADE_STAGE_SOURCE_NETWORK,   /* 来源：网络下载 */
            .epoch = epoch,
        };

        /* ① Begin */
        uint32_t session_id = 0;
        s_phase = 1;
        report_status(req.task_id, "downloading", NULL);

        UpgradeStageResult ret = UpgradeEntry_Begin(&peer, NULL, req.size,
                                                    req.sha256, &session_id);
        if (ret != UPGRADE_STAGE_OK)
        {
            printf("[NWUPG] Begin fail: %s\r\n", UpgradeEntry_ErrorName(ret));
            report_status(req.task_id, "failed", UpgradeEntry_ErrorName(ret));
            s_phase = 6;
            cleanup_attempt(&peer);
            continue;
        }
        printf("[NWUPG] Begin OK: session=%u epoch=%u\r\n", (unsigned)session_id, (unsigned)epoch);

        /* ② 下载（回调里调 Write 写 Flash）
         * 构造下载参数：URL + auth_token + 回调 + chunk_size */
        nw_download_ctx_t dl_ctx =
        {
            .peer = &peer,
            .session_id = session_id,
            .task_id = req.task_id,
            .total_size = req.size,
        };

        int32_t lease_ms = (int32_t)(s_authorized_until_ms - HAL_GetTick());
        if (lease_ms <= 0)
        {
            cleanup_attempt(&peer);
            continue;
        }
        nw_dl_params_t dl_params =
        {
            .url = req.url,
            .auth_token = (req.token[0] != '\0') ? req.token : NULL,
            .total_size = req.size,
            .cb = download_chunk_callback,            /* 每块回调写 Flash */
            .ctx = &dl_ctx,
            .timeout_ms = lease_ms < 120000 ? (uint32_t)lease_ms : 120000U,
            .chunk_size = UPGRADE_STAGE_MAX_CHUNK_SIZE,
        };

        nw_dl_status_t dl_ret = NetworkDownloader_Fetch(&dl_params);
        AppMemory_Report("network-download");
        if (dl_ret != NW_DL_OK)
        {
            printf("[NWUPG] download fail: %s\r\n", NetworkDownloader_ErrorString(dl_ret));
            report_status(req.task_id, "failed", NetworkDownloader_ErrorString(dl_ret));
            /* 取消会话 */
            s_phase = 6;
            cleanup_attempt(&peer);
            continue;
        }

        /* ③ Finish：M4 校验 SHA256 + 签名 → 提交 READY 状态 → 暂停业务
         * Finish 成功 = 固件已完整写入 Flash 且校验通过，可以重启了 */
        s_phase = 2;
        report_status(req.task_id, "verifying", NULL);

        ret = UpgradeEntry_Finish(&peer, session_id);
        AppMemory_Report("network-finish");
        if (ret != UPGRADE_STAGE_OK)
        {
            printf("[NWUPG] Finish fail: %s\r\n", UpgradeEntry_ErrorName(ret));
            report_status(req.task_id, "failed", UpgradeEntry_ErrorName(ret));
            s_phase = 6;
            cleanup_attempt(&peer);
            continue;
        }

        printf("[NWUPG] Finish OK: READY committed, rebooting in 1s...\r\n");
        s_phase = 3;
        report_status(req.task_id, "ready", NULL);
        osDelay(1000);   /* 给云端 1s 收到 status 的时间 */

        /* ④ Reboot：M4 重启设备
         * 重启后 Boot 核验新固件：通过 → 启动新 App → 上报 trial/confirmed
         *                          失败 → 回滚旧固件 → 上报 rolled_back
         * Reboot 成功不会返回（设备已重启），返回则说明失败 */
        s_phase = 4;
        report_status(req.task_id, "rebooting", NULL);
        ret = UpgradeEntry_Reboot(&peer, session_id);
        /* Reboot 成功不返回；若返回则失败 */
        printf("[NWUPG] Reboot returned: %s (should not happen)\r\n",
               UpgradeEntry_ErrorName(ret));
        report_status(req.task_id, "failed", "E_REBOOT");

        cleanup_attempt(&peer);
    }
}

/* ===== 启动结果上报 =====
 * 设备重启后，Boot 核验新固件的结果（trial/confirmed/rolled_back）需要上报给云端。
 * 由升级任务空闲时调用（每秒检查一次），通过 QoS1 发到 ota_status topic。
 * 只消费M4已核验的本次启动快照；同一结果在broker确认前保留并重试。
 *
 * Boot 核验结果含义：
 *   trial        = 新固件启动成功但未确认（试运行，可回滚）
 *   confirmed    = 新固件确认安装（App 主动确认后不可回滚）
 *   rolled_back  = 新固件启动失败，Boot 自动回滚到旧固件 */
static void report_boot_result(void)
{
    if (s_boot_result_done) return;   /* 已上报成功，不重复 */

    /* 从 M4 获取本次启动的身份信息（tx_id + sequence + digest） */
    UpgradeMessage identity;
    if (!App_UpgradeGetStartupIdentity(&identity))
    {
        s_boot_result_done = true;   /* 无升级身份 = 非升级启动，不需要上报 */
        return;
    }
    AppUpgradeResult result = App_UpgradeGetResult();
    if ((result == APP_UPGRADE_RESULT_UNKNOWN) || !MqttClient_IsConnected()) return;

    const char *phase;
    switch (result)
    {
        case APP_UPGRADE_RESULT_TRIAL:        /* 试运行 */
            phase = "trial";
            s_phase = 7;
            break;
        case APP_UPGRADE_RESULT_CONFIRMED:    /* 确认安装 */
            phase = "confirmed";
            s_phase = 5;
            break;
        case APP_UPGRADE_RESULT_ROLLED_BACK:  /* 回滚 */
            phase = "rolled_back";
            s_phase = 8;
            break;
        default:
            return;
    }

    /* digest(32B) → hex 字符串(64字符)，用于云端校验固件指纹 */
    char digest[65];
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(identity.digest); i++)
    {
        digest[i * 2] = hex[identity.digest[i] >> 4];
        digest[i * 2 + 1] = hex[identity.digest[i] & 0x0fU];
    }
    digest[64] = '\0';

    /* 拼启动结果 JSON：
     * {"type":"boot_result","tx_id":"...","sequence":N,"phase":"trial","image_sha256":"..."} */
    char payload[256];
    int len = snprintf(payload, sizeof(payload),
        "{\"type\":\"boot_result\",\"tx_id\":\"%08lx%08lx\",\"sequence\":%lu,"
        "\"phase\":\"%s\",\"image_sha256\":\"%s\"}",
        (unsigned long)(identity.tx_id >> 32), (unsigned long)(uint32_t)identity.tx_id,
        (unsigned long)identity.sequence, phase, digest);
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) return;

    /* 用 PublishUpgradeResult 发送（复用 QoS1 槽，等 PUBACK 确认）
     * 返回 1 = PUBACK 已到（broker 确认），标记完成不再重发 */
    if (MqttClient_PublishUpgradeResult((const uint8_t *)payload, (size_t)len) == 1)
    {
        s_boot_result_done = true;
        printf("[NWUPG] boot result PUBACK: tx=%08lx%08lx phase=%s\r\n",
               (unsigned long)(identity.tx_id >> 32), (unsigned long)(uint32_t)identity.tx_id, phase);
    }
}
