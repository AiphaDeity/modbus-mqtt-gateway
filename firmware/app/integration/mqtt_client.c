/*
 * mqtt_client.c —— M2 上云模块 MQTT 3.1.1 客户端实现（V2 补全版）
 *
 * 对照《系统设计说明书》5 节 & 《任务分工》M2-01~M05 补全：
 *   1. 订阅 commands + acks 双 topic，等 SUBACK（含两个 return code）成功才业务在线
 *   2. CONNECT 带用户名/密码认证（设计 5.1）
 *   3. 业务回执处理：订阅 acks topic，解析 {"record_ids":[...]} → 调 storage_m2_ack 回收缓存（M2-04）
 *   4. 状态/LWT retain=true，其余 retain=false（设计 5.1）
 *   5. 重连退避 1/2/4/8/16/32/60s + 20% 抖动，最长 72s（设计 5.1）
 *   6. 拒绝 RETAIN=1 的保留控制回放（设计 5.1）
 *   7. 补传每次取最多 4 条（内存收缩：原 10→4，STORAGE_M2_FETCH_BATCH_MAX=4）
 *   8. payload 最大 1024B（内存收缩：原 4096→1024，超长报文明确拒绝不截断）
 *   9. 回执 30s 未到则保持原 ID 重发；PUBACK 不回收缓存（设计 5.2）
 *
 * 数据流：
 *   上行(遥测): telemetry_task 采 Modbus -> JSON -> M3缓存队列 + 在线时调 MqttClient_Publish 直发
 *   上行(补传): MQTT任务 replay_pump <- M3 fetch_resp 队列 -> PUBLISH 历史 JSON（不立即 ack 缓存）
 *   业务回执: broker -> acks 主题 -> 解析 record_ids -> storage_m2_ack 回收缓存（30s 未到重发）
 *   下行(命令): broker -> commands 主题 -> 解析 JSON -> mb_inject_write -> Modbus -> 回 cmd_ack
 */
#include "mqtt_client.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>     /* atoi / rand / srand */
#include <stdbool.h>

#include "cmsis_os2.h"
#include "app_monitor.h"
#include "upgrade_quiesce.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/opt.h"
#include "lwip/api.h"        /* netconn_* API */
#include "lwip/netbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"      /* netif_default / netif_is_up / netif_ip4_addr */

#include "app_config.h"      /* AppConfig_Get() 读取 MQTT 配置 */
#include "ethernet.h"        /* m2_ethernet_link_wait() */
#include "mb_api.h"          /* mb_inject_write() 下行写寄存器 */
#include "storage_m2.h"      /* storage_m2_fetch/ack 队列协议（补传） */
#include "storage_record.h"  /* record_out_t / RECORD_KIND_* */
#include "storage_seq.h"     /* storage_seq_alloc() */
#include "storage_time.h"   /* storage_get_unix_time() 取 RTC 时间戳做百度云签名 */
#include "lwip/dns.h"        /* dns_gethostbyname 解析云平台域名 */
#include "hmac_sha256.h"     /* 百度云签名 SHA256 */
#if GATEWAY_ENABLE_M2
#include "network_upgrade.h" /* NetworkUpgrade_Notify() OTA 升级通知分流 */
#endif

/* ===== 上云模式选择 =====
 *   MQTT_USE_BAIDU=1：连百度云 IoT Core（域名 broker + SHA256 签名 + 自定义 topic）
 *   MQTT_USE_BAIDU=0：连本地 mosquitto（IP broker + 明文认证 + client_id/子名 topic）
 * 切换只需改这个宏。百度云模式下三元组填下面三个宏（百度智能云控制台开通后获得）。
 * 百度云 1883 TCP 明文接入，免 TLS，每月免费 100 万条消息，适合无 TLS 的 STM32 设备。 */
#define MQTT_USE_BAIDU      1

#if MQTT_USE_BAIDU
/* ★百度云 IoT Core 三元组 —— 在百度智能云「IoT Core → 设备详情」里复制粘贴到这里。
 *   IoTCoreId / DeviceKey / DeviceSecret 在设备详情页「连接信息」里可见。
 *   broker 域名 = {IoTCoreId}.iot.{region}.baidubce.com，端口 1883（TCP 明文，免 TLS） */
#define BAIDU_IOTCORE_ID    "aeklwww"
#define BAIDU_DEVICE_KEY    "stm32_gw_01"
#define BAIDU_DEVICE_SECRET "YOUR_DEVICE_SECRET"  /* 占位符，测试时替换为真实密钥 */
#define BAIDU_REGION        "gz"        /* 广州地域，创建实例时选的 */
#endif

/* ===== 默认配置（AppConfig 未就绪时回退用，仅本地模式生效） ===== */

#define MQTT_DEF_BROKER_IP0   192
#define MQTT_DEF_BROKER_IP1   168
#define MQTT_DEF_BROKER_IP2   2
#define MQTT_DEF_BROKER_IP3   45
#define MQTT_DEF_BROKER_PORT  1883
#define MQTT_DEF_CLIENT_ID    "mb2mqtt_001"
#define MQTT_DEF_KEEPALIVE    30      /* 秒 */
#define MQTT_SUBACK_TIMEOUT_MS 2000   /* 等 SUBACK 超时 */
#define MQTT_CONNACK_TIMEOUT_MS 5000  /* 等 CONNACK 超时（原 3000 改 5000，给云端更多处理时间） */
#define MQTT_REPLAY_INTERVAL_MS 3000  /* 补传 pump 调度间隔（有新数据时） */
#define MQTT_REPLAY_RESEND_MS  30000  /* 回执未到重发间隔（设计 5.2: 30s） */
#define MQTT_MAX_PAYLOAD       1024   /* 内存收缩：原 4096→1024；超长报文拒绝不截断 */
/* 收发缓冲 = 载荷 + MQTT 头开销（固定头5 + topic_len2 + topic≤96 + packet_id2 ≈ 105），给 200 余量 */
#define MQTT_TX_BUF_SIZE       (MQTT_MAX_PAYLOAD + 200)
#define MQTT_RX_BUF_SIZE       (MQTT_MAX_PAYLOAD + 200)
#define MQTT_IO_TIMEOUT_MS    10000U
#define MQTT_TX_WAIT_MS       12000U
#define MQTT_HEALTH_MARGIN_MS  1000U

/* ===== QoS1 未确认队列（设计 5.2: PUBLISH 带 packet_id → 等 PUBACK → 30s 超时重发）===== */
#define MQTT_QOS1_PENDING_MAX   2     /* 同时最多 2 条未确认（RAM 约束） */
#define MQTT_QOS1_PAYLOAD_MAX   512   /* QoS1 pending payload 上限 */
/* MQTT_REPLAY_RESEND_MS=30000 复用为 QoS1 重发超时（设计 5.2: 30s） */

typedef struct {
    bool        in_use;
    uint16_t    packet_id;
    uint32_t    send_tick;
    bool        retain;
    char        topic[96];
    uint8_t     payload[MQTT_QOS1_PAYLOAD_MAX];
    size_t      payload_len;
} qos1_pending_t;

/* 放 CCMRAM：2条×~620B ≈ 1240B，不占普通 SRAM（设计 5.2: PUBACK 只释放 packet_id） */
static qos1_pending_t s_qos1_pending[MQTT_QOS1_PENDING_MAX] __attribute__((section(".bss.ccmram")));

/* 重连退避表（设计 5.1: 1/2/4/8/16/32/60s + 20% 抖动，最长 72s） */
static const uint32_t g_backoff_table[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000};
#define BACKOFF_TABLE_LEN  (sizeof(g_backoff_table) / sizeof(g_backoff_table[0]))

/* topic 子主题名（设计 5.3: 设备发布 telemetry/events/status，订阅 commands/acks） */
#define MQTT_TOPIC_STATUS     "status"
#define MQTT_TOPIC_EVENTS     "events"
#define MQTT_TOPIC_TELEMETRY  "telemetry"
#define MQTT_TOPIC_COMMANDS   "commands"
#define MQTT_TOPIC_ACKS       "acks"
#define MQTT_TOPIC_CMD_ACK    "cmd_ack"   /* 命令执行回执（设备发布） */
#define MQTT_TOPIC_OTA_STATUS "ota_status" /* OTA 升级状态上报（设备发布） */

/* ===== 运行时配置（Init 时加载） ===== */
typedef struct
{
    char     broker_host[64];    /* 域名或 IP（阿里云域名 / 本地 IP） */
    uint16_t broker_port;
    ip_addr_t broker_ip;         /* DNS 解析后的 IP（连接用） */
    bool     broker_ip_valid;    /* DNS 解析成功标志 */
    char     client_id[48];      /* 百度云: DeviceKey；本地: 自定义 */
    char     username[96];       /* 百度云: thingidp@id|key|ts|SHA256 较长，加宽 */
    char     password[72];       /* 百度云: SHA256 hex=64字符；本地: 明文密码 */
    uint16_t keepalive_sec;
} mqtt_runtime_cfg_t;

static mqtt_runtime_cfg_t g_cfg;

/* topic 缓冲（Init 时按 client_id 拼好） */
static char g_topic_status[96];
static char g_topic_events[96];
static char g_topic_telemetry[96];
static char g_topic_commands[96];
static char g_topic_acks[96];
static char g_topic_cmd_ack[96];
static char g_topic_ota_status[96];

/* ===== 收发缓冲（放 CCMRAM，不挤占 SRAM 里的 FreeRTOS 堆）
 * 内存收缩：tx_buf 4200→1224、rx_ring 4400→1224（载荷上限 1024 + 头开销 200）
 * 注意：netconn_write 用 NETCONN_COPY（拷贝到 LWIP 内部 pbuf），tx_buf 本身不需 DMA 可达，
 *       放 CCMRAM 安全；rx_ring/cmd_payload/ack_payload 全是 CPU 处理，同样安全 */
static uint8_t tx_buf[MQTT_TX_BUF_SIZE]  __attribute__((section(".bss.ccmram")));
static uint8_t rx_ring[MQTT_RX_BUF_SIZE] __attribute__((section(".bss.ccmram")));
static uint16_t rx_ring_len = 0;   /* rx_ring 当前已有效字节数 */

/* 下行命令解析缓冲（PUBLISH 载荷拷贝出来，\0 结尾好给字符串解析） */
static char cmd_payload_buf[MQTT_MAX_PAYLOAD + 16] __attribute__((section(".bss.ccmram")));

/* 业务回执 payload 缓冲 */
static char ack_payload_buf[MQTT_MAX_PAYLOAD + 16] __attribute__((section(".bss.ccmram")));

/* ===== 全局状态 ===== */
static struct netconn *g_conn = NULL;
static volatile bool   g_connected = false;
static volatile bool   g_connect_done;
static osThreadId_t    g_mqtt_thread;
static volatile bool g_pause_requested;
static bool g_pause_acknowledged;

static void mqtt_pause_checkpoint(void);

/* 发送互斥锁：保护 tx_buf + netconn_write，多任务并发安全 */
static osMutexId_t     g_tx_mutex;
static const osMutexAttr_t g_tx_mutex_attr = { .name = "mqtt_tx" };

/* 下行写寄存器的同步信号量（mb_inject_write 用） */
static osSemaphoreId_t g_write_sem;
static mb_errot_t      g_write_err;

/* 下一个 MQTT packet id（SUBSCRIBE 用，QoS1 报文需要） */
static uint16_t g_next_packet_id = 1;
/* 本次启动结果只占用原QoS1槽；由发送锁保护，断线只撤销未确认的packet_id。 */
static uint16_t g_boot_result_packet_id;
static bool g_boot_result_acked;

/* 重连退避计数器（失败次数，成功后清零） */
static uint32_t g_backoff_attempt = 0;

/* 补传回执追踪：记录上次发送的最大 seq 和时间，用于 30s 重发判定 */
static uint64_t g_replay_last_seq = 0;
static uint32_t g_replay_last_tick = 0;

/* 只由MQTT任务更新自身进展；其他发布调用方自行监督等待。 */
static bool mqtt_health_progress(void)
{
    if (osThreadGetId() != g_mqtt_thread)
        return true;
    if (Monitor_ReportProgress(APP_MODULE_MQTT, true))
        return true;
    Monitor_ReportFatal(APP_MODULE_MQTT);
    return false;
}

/* 声明一次阻塞操作的期限，返回后必须结束本次等待。 */
static bool mqtt_health_wait(uint32_t timeout_ms)
{
    if (osThreadGetId() != g_mqtt_thread)
        return true;
    if (Monitor_BeginWait(APP_MODULE_MQTT, timeout_ms + MQTT_HEALTH_MARGIN_MS))
        return true;
    Monitor_ReportFatal(APP_MODULE_MQTT);
    return false;
}

/* 保留原退避总时长，每秒核对单调期限，允许监督发现等待中的停滞。 */
static bool mqtt_delay(uint32_t delay_ms)
{
    uint32_t started_ms = HAL_GetTick();
    for (;;)
    {
        mqtt_pause_checkpoint();
        uint32_t elapsed = (uint32_t)(HAL_GetTick() - started_ms);
        if (elapsed >= delay_ms)
            return true;
        uint32_t remaining = delay_ms - elapsed;
        uint32_t step_ms = remaining < 1000U ? remaining : 1000U;
        if (!mqtt_health_wait(step_ms) || osDelay(pdMS_TO_TICKS(step_ms)) != osOK)
            return false;
        if (!mqtt_health_progress())
            return false;
    }
}

/* 有界获取现有发送锁，失败不继续访问共享发送缓冲。 */
static bool mqtt_lock_tx(void)
{
    if (!mqtt_health_wait(MQTT_TX_WAIT_MS))
        return false;
    osStatus_t status = osMutexAcquire(g_tx_mutex, pdMS_TO_TICKS(MQTT_TX_WAIT_MS));
    if (status != osOK && status != osErrorTimeout)
    {
        if (osThreadGetId() == g_mqtt_thread)
            Monitor_ReportFatal(APP_MODULE_MQTT);
        return false;
    }
    bool healthy = mqtt_health_progress();
    if (status == osOK && healthy)
        return true;
    if (status == osOK)
        osMutexRelease(g_tx_mutex);
    return false;
}

/* 发送完成或失败均结束等待；发送失败关闭准入，避免后续帧接在半包之后。 */
static err_t mqtt_write(const void *data, size_t size)
{
    if (!mqtt_health_wait(MQTT_IO_TIMEOUT_MS))
    {
        g_connected = false;
        return ERR_ABRT;
    }
    err_t err = netconn_write(g_conn, data, size, NETCONN_COPY);
    if (!mqtt_health_progress())
        err = ERR_ABRT;
    if (err != ERR_OK)
        g_connected = false;
    return err;
}

/* 接收至多等待原有的1秒，超时返回由状态机处理，不能靠调用本身续期。 */
static err_t mqtt_receive(struct netbuf **buffer)
{
    *buffer = NULL;
    if (!mqtt_health_wait(1000U))
        return ERR_ABRT;
    err_t err = netconn_recv(g_conn, buffer);
    if (!mqtt_health_progress())
    {
        if (*buffer != NULL)
        {
            netbuf_delete(*buffer);
            *buffer = NULL;
        }
        g_connected = false;
        return ERR_ABRT;
    }
    return err;
}

/* LwIP回调只记录连接结果到达，不替MQTT任务报告健康。 */
static void mqtt_netconn_event(struct netconn *conn, enum netconn_evt event, u16_t size)
{
    (void)size;
    if (conn == g_conn && (event == NETCONN_EVT_SENDPLUS || event == NETCONN_EVT_ERROR))
        g_connect_done = true;
}

/* 非阻塞发起连接，最多10秒；只在任务中检查结果并推进原连接状态机。 */
static err_t mqtt_connect(void)
{
    g_connect_done = false;
    netconn_set_nonblocking(g_conn, 1);
    if (!mqtt_health_wait(1000U))
    {
        netconn_set_nonblocking(g_conn, 0);
        return ERR_TIMEOUT;
    }
    uint32_t started_ms = HAL_GetTick();
    err_t err = netconn_connect(g_conn, &g_cfg.broker_ip, g_cfg.broker_port);
    if (!mqtt_health_progress())
        err = ERR_TIMEOUT;
    while (err == ERR_INPROGRESS)
    {
        if (g_connect_done)
        {
            err = netconn_err(g_conn);
            break;
        }
        uint32_t elapsed = (uint32_t)(HAL_GetTick() - started_ms);
        if (elapsed >= MQTT_IO_TIMEOUT_MS)
        {
            err = ERR_TIMEOUT;
            break;
        }
        uint32_t remaining = MQTT_IO_TIMEOUT_MS - elapsed;
        if (!mqtt_delay(remaining < 100U ? remaining : 100U))
        {
            err = ERR_TIMEOUT;
            break;
        }
    }
    netconn_set_nonblocking(g_conn, 0);
    return err;
}

/* ===== 配置加载 ===== */
static void mqtt_config_load(void)
{
#if MQTT_USE_BAIDU
    /* ===== 百度云模式：broker 域名 + SHA256 签名 =====
     * clientId=DeviceKey, username=thingidp@...|...|ts|SHA256, password=SHA256 hex
     * 每次连接前由 baidu_build_auth() 重算（timestamp 变化） */
    snprintf(g_cfg.broker_host, sizeof(g_cfg.broker_host),
             "%s.iot.%s.baidubce.com", BAIDU_IOTCORE_ID, BAIDU_REGION);
    g_cfg.broker_port = 1883;
    g_cfg.broker_ip_valid = false;     /* 连接时才 DNS 解析 */
    g_cfg.keepalive_sec = MQTT_DEF_KEEPALIVE;
#else
    /* ===== 本地 mosquitto 模式：IP/域名 + 明文认证 ===== */
    const app_config_t *cfg = AppConfig_Get();
    if (cfg != NULL && cfg->magic == CONFIG_MAGIC && cfg->mq_broker_host[0] != '\0')
    {
        strncpy(g_cfg.broker_host, cfg->mq_broker_host, sizeof(g_cfg.broker_host)-1);
        g_cfg.broker_host[sizeof(g_cfg.broker_host)-1] = '\0';
        g_cfg.broker_port = cfg->mq_broker_port ? cfg->mq_broker_port : MQTT_DEF_BROKER_PORT;
        strncpy(g_cfg.client_id, cfg->mq_client_id, sizeof(g_cfg.client_id)-1);
        g_cfg.client_id[sizeof(g_cfg.client_id)-1] = '\0';
        strncpy(g_cfg.username, cfg->mq_username, sizeof(g_cfg.username)-1);
        g_cfg.username[sizeof(g_cfg.username)-1] = '\0';
        strncpy(g_cfg.password, cfg->mq_password, sizeof(g_cfg.password)-1);
        g_cfg.password[sizeof(g_cfg.password)-1] = '\0';
        g_cfg.keepalive_sec = 30;
        g_cfg.broker_ip_valid = false;   /* 统一走 DNS 解析（host 可能是域名或 IP） */
        return;
    }
    /* 回退默认值 */
    strncpy(g_cfg.broker_host, "192.168.2.45", sizeof(g_cfg.broker_host)-1);
    g_cfg.broker_host[sizeof(g_cfg.broker_host)-1] = '\0';
    g_cfg.broker_port  = MQTT_DEF_BROKER_PORT;
    strncpy(g_cfg.client_id, MQTT_DEF_CLIENT_ID, sizeof(g_cfg.client_id)-1);
    g_cfg.client_id[sizeof(g_cfg.client_id)-1] = '\0';
    g_cfg.username[0] = '\0';
    g_cfg.password[0] = '\0';
    g_cfg.keepalive_sec = MQTT_DEF_KEEPALIVE;
    g_cfg.broker_ip_valid = false;
#endif
}

#if MQTT_USE_BAIDU

/* 百度云 IoT Core 认证参数生成（每次连接前调用，timestamp 变化需重算）
 *
 * 百度云官方签名算法（见 cloud.baidu.com/doc/IoTCore MQTT连接）：
 *   clientId   = DeviceKey
 *   username   = "thingidp@" + IoTCoreId + "|" + DeviceKey + "|" + timestamp + "|SHA256"
 *   password   = lowercase_hex( SHA256( DeviceKey + "&" + timestamp + "&SHA256" + DeviceSecret ) )
 *
 *   timestamp 以秒为单位，与服务器时间误差允许 10min；
 *   RTC 未校时时传 0 占位（百度云允许）。
 *
 * broker 地址 = {IoTCoreId}.iot.{region}.baidubce.com:1883（TCP 明文，免 TLS）
 * topic 完全自定义（无固定前缀），权限由设备 topic 列表控制。
 */
/* ★★编译时间 → Unix 时间戳（替代硬编码 base timestamp）
 * __DATE__ 格式 "Mmm dd yyyy"（如 "Nov 10 2026"）
 * __TIME__ 格式 "hh:mm:ss"（如 "17:59:30"）
 * 每次构建自动获得新鲜时间戳，板子烧录后 + uptime 秒数 ≈ 实际时间，
 * 只要编译到烧录间隔 <10 分钟就在百度云允许窗口内。 */
static uint32_t compile_time_to_unix(void)
{
    /* 月份缩写查找表 */
    static const char *const months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *mstr = __DATE__;   /* "Mmm dd yyyy" */
    const char *tstr = __TIME__;   /* "hh:mm:ss"    */

    /* 解析月份 */
    char mon3[4] = {0};
    mon3[0] = mstr[0]; mon3[1] = mstr[1]; mon3[2] = mstr[2];
    const char *p = strstr(months, mon3);
    int month = p ? (int)((p - months) / 3 + 1) : 1;

    /* 解析日、年 */
    int day = atoi(mstr + 4);       /* 跳过 "Mmm " */
    int year = atoi(mstr + 7);      /* 跳过 "Mmm dd " */

    /* 解析时:分:秒 */
    int hour = atoi(tstr);
    int min = atoi(tstr + 3);
    int sec = atoi(tstr + 6);

    /* 累加天数：1970→year-1 的完整年 */
    uint32_t days = 0;
    for (int y = 1970; y < year; y++)
        days += ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 366 : 365;

    /* 累加当年 1→month-1 的完整月 */
    static const int dom[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < month; m++)
    {
        days += dom[m - 1];
        if (m == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)))
            days += 1;  /* 闰年 2 月多一天 */
    }
    days += (uint32_t)(day - 1);

    /* ★★关键：__DATE__/__TIME__ 返回编译机的本地时间（北京 UTC+8），
     * Unix 时间戳是 UTC，必须减去时区偏移。否则时间戳偏前 8 小时，
     * 百度云认证失败返回 CONNACK rc=2。 */
    return days * 86400U + (uint32_t)(hour * 3600 + min * 60 + sec) - 28800U;
}

static void baidu_build_auth(void)
{
    const char *iotcore_id = BAIDU_IOTCORE_ID;
    const char *dk = BAIDU_DEVICE_KEY;
    const char *ds = BAIDU_DEVICE_SECRET;

    /* timestamp：百度云官方文档明确允许传 0 占位（跳过时间校验）。
     * ★★2026-11-10：之前用编译时间+uptime 算 timestamp，但编译本地时间(UTC+8)→UTC
     * 转换有偏差导致认证失败(rc=2)。直接传 0 最安全——服务器跳过时间校验。
     * 后续 SNTP 校时后再改回真实时间戳。 */
    uint32_t ts = 0;  /* 0 = 不校验时间（百度云允许） */

    /* username = thingidp@{IoTCoreId}|{DeviceKey}|{timestamp}|SHA256 */
    snprintf(g_cfg.username, sizeof(g_cfg.username),
             "thingidp@%s|%s|%lu|SHA256",
             iotcore_id, dk, (unsigned long)ts);

    /* 加密字符串 = {DeviceKey}&{timestamp}&SHA256{DeviceSecret} */
    char sign_str[128];
    int n = snprintf(sign_str, sizeof(sign_str),
                     "%s&%lu&SHA256%s", dk, (unsigned long)ts, ds);

    /* password = SHA256(加密字符串) 的 hex 小写 */
    sha256_hex((const uint8_t *)sign_str, (size_t)n, g_cfg.password);

    /* clientId = DeviceKey */
    strncpy(g_cfg.client_id, dk, sizeof(g_cfg.client_id)-1);
    g_cfg.client_id[sizeof(g_cfg.client_id)-1] = '\0';

    printf("[MQTT] Baidu auth: cid=%s ts=%lu\r\n",
           g_cfg.client_id, (unsigned long)ts);
}
#endif

/* 对外：返回遥测主题（telemetry_task 用，替代硬编码 topic，切云平台自动适配） */
const char *MqttClient_TopicTelemetry(void)
{
    return g_topic_telemetry;
}

const char *MqttClient_TopicOtaStatus(void)
{
    return g_topic_ota_status;
}

/* 拼所有主题。
 * 百度云 IoT Core：$iot/{DeviceKey}/user/{子名}（官方固定前缀，详见 IoT Core 文档）
 * 本地 mosquitto：{client_id}/{子名}
 * （百度云 topic 权限在设备详情→Topic 权限里配置，或用通配符 $iot/{DeviceKey}/user/# 授权） */
static void mqtt_topics_build(void)
{
#if MQTT_USE_BAIDU
    /* 百度云：$iot/{DeviceKey}/user/{子名}。g_cfg.client_id 在 baidu_build_auth 里已设为 DeviceKey */
    const char *dk = g_cfg.client_id;
    snprintf(g_topic_status,    sizeof(g_topic_status),    "$iot/%s/user/%s", dk, MQTT_TOPIC_STATUS);
    snprintf(g_topic_events,    sizeof(g_topic_events),    "$iot/%s/user/%s", dk, MQTT_TOPIC_EVENTS);
    snprintf(g_topic_telemetry, sizeof(g_topic_telemetry), "$iot/%s/user/%s", dk, MQTT_TOPIC_TELEMETRY);
    snprintf(g_topic_commands,  sizeof(g_topic_commands),  "$iot/%s/user/%s", dk, MQTT_TOPIC_COMMANDS);
    snprintf(g_topic_acks,      sizeof(g_topic_acks),      "$iot/%s/user/%s", dk, MQTT_TOPIC_ACKS);
    snprintf(g_topic_cmd_ack,   sizeof(g_topic_cmd_ack),   "$iot/%s/user/%s", dk, MQTT_TOPIC_CMD_ACK);
    snprintf(g_topic_ota_status,sizeof(g_topic_ota_status),"$iot/%s/user/%s", dk, MQTT_TOPIC_OTA_STATUS);
#else
    snprintf(g_topic_status,    sizeof(g_topic_status),    "%s/%s", g_cfg.client_id, MQTT_TOPIC_STATUS);
    snprintf(g_topic_events,    sizeof(g_topic_events),    "%s/%s", g_cfg.client_id, MQTT_TOPIC_EVENTS);
    snprintf(g_topic_telemetry, sizeof(g_topic_telemetry), "%s/%s", g_cfg.client_id, MQTT_TOPIC_TELEMETRY);
    snprintf(g_topic_commands,  sizeof(g_topic_commands),  "%s/%s", g_cfg.client_id, MQTT_TOPIC_COMMANDS);
    snprintf(g_topic_acks,      sizeof(g_topic_acks),      "%s/%s", g_cfg.client_id, MQTT_TOPIC_ACKS);
    snprintf(g_topic_cmd_ack,   sizeof(g_topic_cmd_ack),   "%s/%s", g_cfg.client_id, MQTT_TOPIC_CMD_ACK);
    snprintf(g_topic_ota_status,sizeof(g_topic_ota_status),"%s/%s", g_cfg.client_id, MQTT_TOPIC_OTA_STATUS);
#endif
}

/* ===== MQTT 变长 remaining length 编码/解码 ===== */

static int mqtt_encode_remaining_length(uint8_t *buf, uint32_t value)
{
    int n = 0;
    do {
        uint8_t b = value & 0x7F;
        value >>= 7;
        if (value > 0) b |= 0x80;
        buf[n++] = b;
    } while (value > 0 && n < 4);
    return n;
}

static int mqtt_decode_remaining_length(const uint8_t *data, uint16_t avail, uint32_t *out_value, int *out_bytes)
{
    uint32_t value = 0;
    int multiplier = 1;
    int i = 0;
    while (i < avail && i < 4)
    {
        uint8_t b = data[i];
        value += (uint32_t)(b & 0x7F) * multiplier;
        i++;
        if ((b & 0x80) == 0)
        {
            *out_value = value;
            *out_bytes = i;
            return 0;
        }
        multiplier *= 128;
    }
    return -1;
}

/* ===== 退避计算（设计 5.1: 1/2/4/8/16/32/60s + 20% 抖动） ===== */
static uint32_t mqtt_backoff_delay(uint32_t attempt)
{
    uint32_t idx = attempt;
    if (idx >= BACKOFF_TABLE_LEN) idx = BACKOFF_TABLE_LEN - 1;
    uint32_t base = g_backoff_table[idx];
    uint32_t jitter = base / 5;   /* 20% */
    if (jitter > 0) jitter = (uint32_t)rand() % jitter;
    return base + jitter;
}

/* ===== MQTT 报文构造（写入 tx_buf，返回总长度） ===== */

/**
 * 组 CONNECT 报文（带遗嘱 LWT + 用户名/密码认证）
 * 设计 5.1: CleanSession=1, Keepalive, LWT retain=true, 认证
 */
static size_t mqtt_build_connect(void)
{
    uint8_t vh[512];
    size_t n = 0;

    /* 可变头 */
    vh[n++] = 0x00; vh[n++] = 0x04;
    vh[n++] = 'M'; vh[n++] = 'Q'; vh[n++] = 'T'; vh[n++] = 'T';
    vh[n++] = 0x04;   /* protocol level 4 = MQTT 3.1.1 */

    /* Connect Flags:
     *   bit0 reserved(0) bit1 CleanSession(1) bit2 WillFlag(1)
     *   bit3-4 WillQoS(0) bit5 WillRetain(1) ← 设计要求状态/LWT retain=true
     *   bit6 PasswordFlag bit7 UserNameFlag
     * 基础: 0x02(Clean) | 0x04(WillFlag) | 0x20(WillRetain) = 0x26
     * 有认证时: | 0x80(UserName) | 0x40(Password) = 0xE6 */
    bool has_auth = (g_cfg.username[0] != '\0');
    uint8_t flags = 0x02 | 0x04 | 0x20;   /* CleanSession + WillFlag + WillRetain */
    if (has_auth) flags |= 0x80 | 0x40;    /* UserNameFlag + PasswordFlag */
    vh[n++] = flags;

    vh[n++] = (uint8_t)((g_cfg.keepalive_sec >> 8) & 0xFF);
    vh[n++] = (uint8_t)(g_cfg.keepalive_sec & 0xFF);

    /* 载荷：ClientID */
    size_t cidlen = strlen(g_cfg.client_id);
    vh[n++] = (uint8_t)((cidlen >> 8) & 0xFF);
    vh[n++] = (uint8_t)(cidlen & 0xFF);
    memcpy(&vh[n], g_cfg.client_id, cidlen);
    n += cidlen;

    /* 载荷：遗嘱主题（status topic，retain=true） */
    const char *will_topic = g_topic_status;
    size_t wtlen = strlen(will_topic);
    vh[n++] = (uint8_t)((wtlen >> 8) & 0xFF);
    vh[n++] = (uint8_t)(wtlen & 0xFF);
    memcpy(&vh[n], will_topic, wtlen);
    n += wtlen;

    /* 载荷：遗嘱消息 */
    const char *will_msg = "offline";
    size_t wmlen = strlen(will_msg);
    vh[n++] = (uint8_t)((wmlen >> 8) & 0xFF);
    vh[n++] = (uint8_t)(wmlen & 0xFF);
    memcpy(&vh[n], will_msg, wmlen);
    n += wmlen;

    /* 载荷：用户名 + 密码（仅有认证时） */
    if (has_auth)
    {
        size_t ulen = strlen(g_cfg.username);
        vh[n++] = (uint8_t)((ulen >> 8) & 0xFF);
        vh[n++] = (uint8_t)(ulen & 0xFF);
        memcpy(&vh[n], g_cfg.username, ulen);
        n += ulen;

        size_t plen = strlen(g_cfg.password);
        vh[n++] = (uint8_t)((plen >> 8) & 0xFF);
        vh[n++] = (uint8_t)(plen & 0xFF);
        memcpy(&vh[n], g_cfg.password, plen);
        n += plen;
    }

    /* 固定头 */
    size_t off = 0;
    tx_buf[off++] = 0x10;   /* CONNECT */
    off += mqtt_encode_remaining_length(&tx_buf[off], (uint32_t)n);
    memcpy(&tx_buf[off], vh, n);
    return off + n;
}

/**
 * 组 SUBSCRIBE 报文（订阅 commands + acks 两个 topic，QoS0）
 * 设计 5.1: 连接成功后订阅 commands 与 acks，检查两个 SUBACK 均成功才业务在线
 * 一个 SUBSCRIBE 报文可含多个 topic filter，SUBACK 回对应数量的 return code
 */
static size_t mqtt_build_subscribe(void)
{
    uint8_t vh[160];
    size_t n = 0;

    uint16_t pid = g_next_packet_id++;
    if (g_next_packet_id == 0) g_next_packet_id = 1;
    vh[n++] = (uint8_t)((pid >> 8) & 0xFF);
    vh[n++] = (uint8_t)(pid & 0xFF);

    /* topic 1: commands */
    size_t tlen1 = strlen(g_topic_commands);
    vh[n++] = (uint8_t)((tlen1 >> 8) & 0xFF);
    vh[n++] = (uint8_t)(tlen1 & 0xFF);
    memcpy(&vh[n], g_topic_commands, tlen1);
    n += tlen1;
    vh[n++] = 0x00;   /* QoS0 */

    /* topic 2: acks */
    size_t tlen2 = strlen(g_topic_acks);
    vh[n++] = (uint8_t)((tlen2 >> 8) & 0xFF);
    vh[n++] = (uint8_t)(tlen2 & 0xFF);
    memcpy(&vh[n], g_topic_acks, tlen2);
    n += tlen2;
    vh[n++] = 0x00;   /* QoS0 */

    size_t off = 0;
    tx_buf[off++] = 0x82;   /* SUBSCRIBE */
    off += mqtt_encode_remaining_length(&tx_buf[off], (uint32_t)n);
    memcpy(&tx_buf[off], vh, n);
    return off + n;
}

/**
 * 组 PUBLISH 报文（支持 QoS0/QoS1，DUP flag，retain 可选）
 * 设计 5.1: 状态 retain=true，其余 retain=false
 * 设计 5.2: OTA状态/补传用 QoS1（带 packet_id + 等 PUBACK + 30s 重发）
 */
static size_t mqtt_build_publish(const char *topic, const uint8_t *payload, size_t plen,
                                 bool retain, uint8_t qos, uint16_t packet_id, bool dup)
{
    size_t tlen = strlen(topic);
    size_t remaining = 2 + tlen + plen;
    if (qos > 0) remaining += 2;  /* QoS1/2 加 2 字节 packet_id */

    size_t off = 0;
    uint8_t header = 0x30 | (retain ? 0x01 : 0x00);
    if (qos == 1) header |= 0x02;        /* QoS1: bit1=1 bit2=0 */
    if (dup)      header |= 0x08;        /* DUP=1: 重发标志 */
    tx_buf[off++] = header;
    off += mqtt_encode_remaining_length(&tx_buf[off], (uint32_t)remaining);
    tx_buf[off++] = (uint8_t)((tlen >> 8) & 0xFF);
    tx_buf[off++] = (uint8_t)(tlen & 0xFF);
    memcpy(&tx_buf[off], topic, tlen);
    off += tlen;
    if (qos > 0)
    {
        tx_buf[off++] = (uint8_t)((packet_id >> 8) & 0xFF);
        tx_buf[off++] = (uint8_t)(packet_id & 0xFF);
    }
    memcpy(&tx_buf[off], payload, plen);
    off += plen;
    return off;
}

/* 组 PINGREQ（固定头 0xC0 0x00） */
static size_t mqtt_build_pingreq(void)
{
    tx_buf[0] = 0xC0;
    tx_buf[1] = 0x00;
    return 2;
}

/* ===== 收包帧重组 ===== */

static void rx_ring_append(const uint8_t *data, size_t len)
{
    /* 超长报文明确拒绝，不截断执行（原实现滑窗保留尾部会导致半帧误解析）。
     * 正常 MQTT 帧 ≤ 载荷1024 + 头~105 ≈ 1129 < ring(1224)，不会触发这里。 */
    if (len >= sizeof(rx_ring))
    {
        printf("[MQTT] drop oversize seg %u > ring %u (rejected, not truncated)\r\n",
               (unsigned)len, (unsigned)sizeof(rx_ring));
        rx_ring_len = 0;
        return;
    }
    if (rx_ring_len + len > sizeof(rx_ring))
    {
        /* 累积溢出：前帧不完整且 ring 将满，说明帧超长或乱序，清空丢弃 */
        printf("[MQTT] drop overflow %u+%u > ring %u (rejected, not truncated)\r\n",
               (unsigned)rx_ring_len, (unsigned)len, (unsigned)sizeof(rx_ring));
        rx_ring_len = 0;
        /* ring 已清空，追加本段（可能是新帧开头）重新同步 */
    }
    memcpy(rx_ring + rx_ring_len, data, len);
    rx_ring_len += len;
}

static size_t rx_ring_take_frame(uint8_t *out_type, uint8_t *out_flags,
                                 const uint8_t **out_body, uint32_t *out_body_len)
{
    if (rx_ring_len < 2) return 0;

    uint32_t remaining = 0;
    int rl_bytes = 0;
    if (mqtt_decode_remaining_length(&rx_ring[1], rx_ring_len - 1, &remaining, &rl_bytes) != 0)
        return 0;

    size_t frame_total = 1 + rl_bytes + remaining;
    /* 超长帧拒绝：单帧总长超过 ring 容量，永远装不下，清空丢弃不截断等待 */
    if (frame_total > sizeof(rx_ring))
    {
        printf("[MQTT] drop oversize frame total=%u > ring %u (rejected)\r\n",
               (unsigned)frame_total, (unsigned)sizeof(rx_ring));
        rx_ring_len = 0;
        return 0;
    }
    if (rx_ring_len < frame_total) return 0;

    *out_type  = (rx_ring[0] >> 4) & 0x0F;
    *out_flags = rx_ring[0] & 0x0F;
    *out_body  = &rx_ring[1 + rl_bytes];
    *out_body_len = remaining;
    return frame_total;
}

static void rx_ring_consume(size_t frame_total)
{
    if (frame_total >= rx_ring_len)
    {
        rx_ring_len = 0;
    }
    else
    {
        memmove(rx_ring, rx_ring + frame_total, rx_ring_len - frame_total);
        rx_ring_len -= frame_total;
    }
}

/* ===== 下行命令处理 =====
 * 收到 PUBLISH 到 commands 主题时调。
 * 设计 5.1: 拒绝 RETAIN=1 的保留控制回放
 */
static void mqtt_handle_command(const uint8_t *body, uint32_t body_len, uint8_t publish_flags)
{
    /* 可变头：主题长度(2) + 主题；载荷：主题之后 */
    if (body_len < 4) return;
    uint16_t tlen = ((uint16_t)body[0] << 8) | body[1];
    if ((uint32_t)(2 + tlen) > body_len) return;

    const char *topic = (const char *)&body[2];
    const uint8_t *payload = body + 2 + tlen;
    uint32_t plen = body_len - 2 - tlen;

    /* RETAIN 检查：设计 5.1 拒绝 RETAIN=1 的保留控制回放 */
    bool is_retain = (publish_flags & 0x01) != 0;
    if (is_retain)
    {
        printf("[MQTT] reject retained command (RETAIN=1)\r\n");
        return;
    }

    /* 只处理 commands 主题 */
    if (tlen != strlen(g_topic_commands) || memcmp(topic, g_topic_commands, tlen) != 0)
    {
        return;
    }

    /* 拷贝载荷到 \0 结尾缓冲 */
    if (plen >= sizeof(cmd_payload_buf)) plen = sizeof(cmd_payload_buf) - 1;
    memcpy(cmd_payload_buf, payload, plen);
    cmd_payload_buf[plen] = '\0';

    printf("[MQTT] recv cmd: %s\r\n", cmd_payload_buf);

#if GATEWAY_ENABLE_M2
    /* ★OTA 升级通知分流：JSON 含 "url" + "sha256" → 升级通知，转给 NetworkUpgrade。
     * 升级通知和普通 Modbus 命令复用 commands topic，靠 JSON 字段区分：
     *   升级通知: {"task_id":"...","url":"http://...","sha256":"...","size":262144,...}
     *   普通命令: {"slave":2,"func":6,"addr":0,"val":100}
     * 不在回调里执行下载/Flash/密码运算（README 要求），只唤醒执行任务。 */
    if (strstr(cmd_payload_buf, "\"url\"") && strstr(cmd_payload_buf, "\"sha256\""))
    {
        int nr = NetworkUpgrade_Notify(cmd_payload_buf);
        char ack[96];
        int alen;
        if (nr == 0)
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"accepted\",\"type\":\"ota\"}");
        }
        else if (nr == -2)
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"busy\",\"type\":\"ota\"}");
        }
        else
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"type\":\"ota\",\"err\":%d}", nr);
        }
        MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)ack, (size_t)alen);
        return;
    }
#endif

    if (g_pause_requested || !UpgradeQuiesce_AllowsMutation())
    {
        static const char busy[] = "{\"status\":\"busy\",\"err\":\"upgrade\"}";
        MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)busy, sizeof(busy) - 1U);
        return;
    }

    /* 手写 JSON 解析：找 "slave":<int> "func":<int> "addr":<int> "val":<int> */
    int slave = -1, func = -1, addr = -1, val = 0;
    const char *keys[] = { "\"slave\"", "\"func\"", "\"addr\"", "\"val\"" };
    int *outs[] = { &slave, &func, &addr, &val };
    for (int k = 0; k < 4; k++)
    {
        const char *p = strstr(cmd_payload_buf, keys[k]);
        if (!p) continue;
        p += strlen(keys[k]);
        while (*p == ' ' || *p == ':' || *p == '\t') p++;
        if (*p == '-' || (*p >= '0' && *p <= '9'))
        {
            *outs[k] = atoi(p);
        }
    }

    char ack[96];
    int alen;

    if (slave < 1 || slave > 247 || func < 0 || addr < 0)
    {
        alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":\"bad_cmd\"}");
        MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)ack, (size_t)alen);
        return;
    }

    uint16_t write_val = (uint16_t)val;
    if (!mqtt_health_wait(3000U)) return;
    mb_status_e st = mb_inject_write((uint8_t)slave,
                                     (uint8_t)func,
                                     (uint16_t)addr,
                                     1,
                                     &write_val,
                                     g_write_sem,
                                     &g_write_err,
                                     3000);
    if (!mqtt_health_progress()) return;

    if (st == MB_STATUS_OK)
    {
        if (!mqtt_health_wait(4000U)) return;
        osStatus_t status = osSemaphoreAcquire(g_write_sem, pdMS_TO_TICKS(4000U));
        if (!mqtt_health_progress()) return;
        if (status == osOK)
        {
            int errcode = (int)g_write_err;
            if (errcode == (int)ERR_OK_QUERY)
            {
                alen = snprintf(ack, sizeof(ack), "{\"status\":\"ok\",\"err\":0}");
            }
            else
            {
                alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":%d}", errcode);
            }
        }
        else
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":\"timeout\"}");
        }
    }
    else if (st == MB_QUEUE_FULL_ERR)
    {
        alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":\"qfull\"}");
    }
    else
    {
        alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":%d}", (int)st);
    }

    MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)ack, (size_t)alen);
}

/* ===== 业务回执处理 =====
 * 收到 PUBLISH 到 acks 主题时调（设计 5.2）。
 * payload: {"schema_version":1,"gateway_id":"mb2mqtt_001","ack_id":"a001","record_ids":["mb2mqtt_001:seq:1001"]}
 * 提取所有 record_id 中的 seq，取最大值，调 storage_m2_ack 回收缓存。
 * PUBACK 不回收数据——只有业务回执才回收（设计 5.2 明确）。
 */
static void mqtt_handle_ack(const uint8_t *body, uint32_t body_len)
{
    if (body_len < 4) return;
    uint16_t tlen = ((uint16_t)body[0] << 8) | body[1];
    if ((uint32_t)(2 + tlen) > body_len) return;

    const char *topic = (const char *)&body[2];
    const uint8_t *payload = body + 2 + tlen;
    uint32_t plen = body_len - 2 - tlen;

    /* 只处理 acks 主题 */
    if (tlen != strlen(g_topic_acks) || memcmp(topic, g_topic_acks, tlen) != 0)
    {
        return;
    }

    /* 拷贝到 \0 结尾缓冲 */
    if (plen >= sizeof(ack_payload_buf)) plen = sizeof(ack_payload_buf) - 1;
    memcpy(ack_payload_buf, payload, plen);
    ack_payload_buf[plen] = '\0';

    /* 解析所有 record_id（格式 "gateway_id:seq:数字"），提取 seq 取最大值 */
    uint64_t max_seq = 0;
    int found = 0;
    const char *p = ack_payload_buf;
    while ((p = strstr(p, ":seq:")) != NULL)
    {
        p += 5;   /* 跳过 ":seq:" */
        uint64_t seq = 0;
        int got_digit = 0;
        while (*p >= '0' && *p <= '9')
        {
            seq = seq * 10 + (uint64_t)(*p - '0');
            p++;
            got_digit = 1;
        }
        if (got_digit)
        {
            if (seq > max_seq) max_seq = seq;
            found = 1;
        }
    }

    if (found && max_seq > 0)
    {
        storage_ack_t ack = { .ack_upto_seq = max_seq };
        taskENTER_CRITICAL();
        bool allowed = !g_pause_requested && UpgradeQuiesce_AllowsMutation();
        osStatus_t status = allowed ? osMessageQueuePut(storage_m2_ack_queue(), &ack, 0, 0) : osErrorResource;
        taskEXIT_CRITICAL();
        if (status != osOK)
        {
            printf("[MQTT] biz ack deferred seq=%lu status=%d\r\n",
                   (unsigned long)max_seq, (int)status);
            return;
        }
        printf("[MQTT] biz ack received, ack_upto_seq=%lu\r\n", (unsigned long)max_seq);

        /* 收到回执说明上次发送被确认，重置重发计时 */
        g_replay_last_seq = 0;
        g_replay_last_tick = 0;
    }
}

/* 收到 PUBACK：匹配 QoS1 pending 队列，释放 packet_id
 * 设计 5.2: PUBACK 只释放 MQTT packet_id，不回收业务缓存（业务回执才回收） */
static void mqtt_handle_puback(uint16_t packet_id)
{
    if (!mqtt_lock_tx())
        return;
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++)
    {
        if (s_qos1_pending[i].in_use && s_qos1_pending[i].packet_id == packet_id)
        {
            if (packet_id == g_boot_result_packet_id)
            {
                g_boot_result_acked = true;
                g_boot_result_packet_id = 0;
            }
            s_qos1_pending[i].in_use = false;
            printf("[MQTT] PUBACK pid=%u\r\n", (unsigned)packet_id);
            break;
        }
    }
    osMutexRelease(g_tx_mutex);
}

/* ===== 离线补传驱动 =====
 * 设计 5.2:
 *   - 在线立即请求存储任务读取已提交记录，按现有 Topic 和 JSON 契约发布
 *   - PUBACK 后仅释放 MQTT packet_id，不回收缓存（业务回执才回收）
 *   - 回执 30s 未到则保持原 ID 重发，重试间隔不超过 60s
 *   - 恢复后每次取最多 4 条（内存收缩：原 10→4）
 */
static bool g_fetch_in_flight = false;
static uint32_t g_fetch_req_id = 1;
static uint32_t g_fetch_expected_id;

/* ★放 static + CCMRAM：storage_fetch_resp_t 含 records[4]×544B ≈ 2.2KB（收缩前 records[10]≈5.4KB），
 * 放栈上会爆任务栈。本函数单线程（仅 mqtt 任务调），static 安全。 */
static storage_fetch_resp_t g_fetch_resp __attribute__((section(".bss.ccmram")));

/* 只接纳匹配的旧请求应答；读取副本不回收存储中的未确认记录。 */
static bool mqtt_take_fetch_response(void)
{
    if (osMessageQueueGet(storage_m2_fetch_resp_queue(), &g_fetch_resp, NULL, 0) != osOK)
        return false;
    if (!g_fetch_in_flight || g_fetch_resp.req_id != g_fetch_expected_id)
    {
        printf("[MQTT] stale fetch response id=%lu expected=%lu\r\n",
               (unsigned long)g_fetch_resp.req_id, (unsigned long)g_fetch_expected_id);
        return false;
    }
    g_fetch_in_flight = false;
    return true;
}

/* 本任务在业务处理之间确认暂停，退避期间也接收已发出的补传应答。 */
static void mqtt_pause_checkpoint(void)
{
    if (g_pause_requested)
        (void)mqtt_take_fetch_response();
    taskENTER_CRITICAL();
    g_pause_acknowledged = g_pause_requested && !g_fetch_in_flight && mqtt_health_progress();
    taskEXIT_CRITICAL();
}

/* 保活、命令拒绝回执及升级状态可继续发送；业务发送复查最终准入。 */
static bool mqtt_allows_publish(const char *topic)
{
    return strcmp(topic, g_topic_status) == 0 || strcmp(topic, g_topic_cmd_ack) == 0 ||
           strcmp(topic, g_topic_ota_status) == 0 ||
           (!g_pause_requested && UpgradeQuiesce_AllowsMutation());
}

/* 设置请求时清除旧确认，防止取消后再次暂停复用旧状态。 */
void MqttClient_SetPaused(bool paused)
{
    taskENTER_CRITICAL();
    g_pause_requested = paused;
    g_pause_acknowledged = false;
    taskEXIT_CRITICAL();
}

/* 只观察任务确认及既有发送锁，不代报MQTT任务健康。 */
bool MqttClient_IsPaused(void)
{
    taskENTER_CRITICAL();
    bool paused = g_pause_requested && g_pause_acknowledged;
    taskEXIT_CRITICAL();
    if (!paused || g_tx_mutex == NULL || osMutexAcquire(g_tx_mutex, 0U) != osOK)
        return false;
    osMutexRelease(g_tx_mutex);
    return true;
}

/* 完成匹配的补传应答后按原协议发送，暂停期间不再发起下一批。 */
static void mqtt_replay_pump(void)
{
    /* 1. 非阻塞检查 M3 的补传应答 */
    if (mqtt_take_fetch_response())
    {
        if (g_pause_requested || !UpgradeQuiesce_AllowsMutation()) return;

        if (g_fetch_resp.count > 0)
        {
            uint64_t max_seq = 0;
            for (int i = 0; i < g_fetch_resp.count; i++)
            {
                if (g_pause_requested || !UpgradeQuiesce_AllowsMutation()) break;
                record_out_t *r = &g_fetch_resp.records[i];
                const char *topic = (r->kind == RECORD_KIND_EVENT) ? g_topic_events : g_topic_telemetry;
                if (MqttClient_PublishQos1(topic, r->payload, r->payload_len) == 0)
                {
                    if (r->seq > max_seq) max_seq = r->seq;
                }
                else
                {
                    break;   /* 发送失败，停掉本轮 */
                }
            }

            /* 记录本轮发送的最大 seq 和时间（不立即 ack 缓存，等业务回执）
             * 设计 5.2: PUBACK 不回收数据，业务回执 30s 未到则重发 */
            if (max_seq > 0)
            {
                /* 如果本轮 max_seq 和上次相同（数据没推进 = 平台没回 ack）：
                 * 检查是否够 30s，不够则跳过本轮重发（避免 3s 一次轰炸） */
                if (max_seq == g_replay_last_seq && g_replay_last_tick > 0)
                {
                    uint32_t elapsed = osKernelGetTickCount() - g_replay_last_tick;
                    if (elapsed < MQTT_REPLAY_RESEND_MS)
                    {
                        return;   /* 还没到 30s，等回执或超时再重发 */
                    }
                }
                g_replay_last_seq = max_seq;
                g_replay_last_tick = osKernelGetTickCount();
                printf("[MQTT] replay %u records, max_seq=%lu (await biz ack)\r\n",
                       (unsigned)g_fetch_resp.count, (unsigned long)max_seq);
            }
        }
        else
        {
            /* count=0：没有更多历史记录，重置追踪 */
            g_replay_last_seq = 0;
            g_replay_last_tick = 0;
        }
    }

    /* 2. 没有在途请求时，发新的拉取请求 */
    if (!g_fetch_in_flight)
    {
        storage_fetch_req_t req = {
            .req_id = g_fetch_req_id,
            .max_count = STORAGE_M2_FETCH_BATCH_MAX,
        };
        taskENTER_CRITICAL();
        bool allowed = !g_pause_requested && UpgradeQuiesce_AllowsMutation();
        if (allowed && osMessageQueuePut(storage_m2_fetch_req_queue(), &req, 0, 0) == osOK)
        {
            g_fetch_in_flight = true;
            g_fetch_expected_id = req.req_id;
            if (++g_fetch_req_id == 0U) g_fetch_req_id = 1U;
        }
        taskEXIT_CRITICAL();
    }
}

/* QoS1 超时重发检查（设计 5.2: 30s 未收到 PUBACK 则保持原 packet_id 重发，DUP=1）
 * 在主循环 ERR_TIMEOUT 分支和补传 pump 一起调用 */
static void mqtt_qos1_check_resend(void)
{
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++)
    {
        if (!mqtt_lock_tx())
            return;
        uint32_t now = osKernelGetTickCount();
        if (!s_qos1_pending[i].in_use || !mqtt_allows_publish(s_qos1_pending[i].topic) ||
            (now - s_qos1_pending[i].send_tick) < MQTT_REPLAY_RESEND_MS)
        {
            osMutexRelease(g_tx_mutex);
            continue;
        }

        /* 30s 超时，重发（保持原 packet_id，DUP=1） */
        size_t pkt_len = mqtt_build_publish(s_qos1_pending[i].topic,
                                            s_qos1_pending[i].payload,
                                            s_qos1_pending[i].payload_len,
                                            s_qos1_pending[i].retain, 1,
                                            s_qos1_pending[i].packet_id, true);
        err_t err = mqtt_write(tx_buf, pkt_len);
        if (err == ERR_OK)
        {
            s_qos1_pending[i].send_tick = now;
            printf("[MQTT] QoS1 resend pid=%u (30s timeout)\r\n",
                   (unsigned)s_qos1_pending[i].packet_id);
        }
        osMutexRelease(g_tx_mutex);
        if (err != ERR_OK) return;
    }
}

/* ===== 对外接口实现 ===== */

AppModuleInitResult MqttClient_Init(void)
{
    if (g_tx_mutex != NULL && g_write_sem != NULL)
        return APP_MODULE_INIT_OK;

    if (g_tx_mutex == NULL)
    {
        g_tx_mutex = osMutexNew(&g_tx_mutex_attr);
        if (g_tx_mutex == NULL)
            return APP_MODULE_INIT_FAILED;
    }

    g_write_sem = osSemaphoreNew(1, 0, NULL);
    if (g_write_sem == NULL)
    {
        if (osMutexDelete(g_tx_mutex) == osOK)
            g_tx_mutex = NULL;
        return APP_MODULE_INIT_FAILED;
    }

    srand(osKernelGetTickCount());   /* 退避抖动随机种子 */

    mqtt_config_load();

#if MQTT_USE_BAIDU
    /* ★★2026-11-10 关键修复：必须先调 baidu_build_auth() 设置 g_cfg.client_id=DeviceKey，
     * 再调 mqtt_topics_build() 拼 topic。之前顺序反了：topics_build 用空 client_id
     * → Will Topic 变成 "$iot//user/status"（DeviceKey 丢失）→ CONNACK rc=2。
     * baidu_build_auth 里的 timestamp 每次连接前会重算（在 task 循环里），这里只是
     * 初始化阶段把 client_id/username/password 填上，让 topics_build 能拿到正确的 DeviceKey。 */
    baidu_build_auth();
#endif

    mqtt_topics_build();

    g_conn = NULL;
    g_connected = false;
    rx_ring_len = 0;
    g_fetch_in_flight = false;
    g_backoff_attempt = 0;
    g_replay_last_seq = 0;
    g_replay_last_tick = 0;
    return APP_MODULE_INIT_OK;
}

bool MqttClient_IsConnected(void)
{
    return g_connected;
}

int MqttClient_Publish(const char *topic, const uint8_t *payload, size_t plen)
{
    if (g_tx_mutex == NULL)
    {
        return -1;
    }

    /* 载荷上限检查（内存收缩：MQTT_MAX_PAYLOAD=1024；超长明确拒绝不截断） */
    if (plen > MQTT_MAX_PAYLOAD)
    {
        return -3;   /* 超过载荷上限 */
    }
    size_t tlen = strlen(topic);
    if (tlen + plen + 8 > sizeof(tx_buf))
    {
        return -3;   /* 报文装不进 tx_buf */
    }

    if (!mqtt_lock_tx())
        return -1;
    if (!g_connected || g_conn == NULL)
    {
        osMutexRelease(g_tx_mutex);
        return -1;
    }

    if (!mqtt_allows_publish(topic))
    {
        osMutexRelease(g_tx_mutex);
        return -5;
    }

    size_t pkt_len = mqtt_build_publish(topic, payload, plen, false, 0, 0, false);   /* QoS0, retain=false */
    err_t err = mqtt_write(tx_buf, pkt_len);

    osMutexRelease(g_tx_mutex);
    return (err == ERR_OK) ? 0 : -2;
}

/* 共用原QoS1发送路径；启动结果等匹配PUBACK后才返回已确认。 */
static int mqtt_publish_qos1(const char *topic, const uint8_t *payload, size_t plen, bool boot_result)
{
    if (g_tx_mutex == NULL)
    {
        return -1;
    }

    /* 载荷上限检查（超长明确拒绝不截断；QoS1 pending 槽 512B 更严） */
    if (plen > MQTT_MAX_PAYLOAD)
    {
        return -3;   /* 超过载荷上限 */
    }
    size_t tlen = strlen(topic);
    if (tlen + plen + 10 > sizeof(tx_buf))
    {
        return -3;   /* 报文装不进 tx_buf */
    }
    if (plen > MQTT_QOS1_PAYLOAD_MAX)
    {
        return -3;   /* 超过 QoS1 pending payload 上限(512B) */
    }

    if (!mqtt_lock_tx())
        return -1;
    if (boot_result && (g_boot_result_acked || (g_boot_result_packet_id != 0U)))
    {
        int result = g_boot_result_acked ? 1 : 0;
        osMutexRelease(g_tx_mutex);
        return result;
    }
    if (!g_connected || g_conn == NULL)
    {
        osMutexRelease(g_tx_mutex);
        return -1;
    }

    /* 找空闲 pending 槽 */
    if (!mqtt_allows_publish(topic))
    {
        osMutexRelease(g_tx_mutex);
        return -5;
    }
    int slot = -1;
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++)
    {
        if (!s_qos1_pending[i].in_use) { slot = i; break; }
    }
    if (slot < 0)
    {
        osMutexRelease(g_tx_mutex);
        return -4;   /* pending 队列满 */
    }

    /* 跳过仍在途的编号，避免回绕后将其他发布的PUBACK当成启动结果确认。 */
    uint16_t pid;
    bool id_in_use;
    do
    {
        pid = g_next_packet_id++;
        if (g_next_packet_id == 0) g_next_packet_id = 1;
        id_in_use = false;
        for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++)
        {
            if (s_qos1_pending[i].in_use && s_qos1_pending[i].packet_id == pid)
                id_in_use = true;
        }
    } while (id_in_use);

    /* 存 pending（拷贝 payload，函数返回后调用方数据可释放） */
    s_qos1_pending[slot].in_use      = true;
    s_qos1_pending[slot].packet_id   = pid;
    s_qos1_pending[slot].send_tick   = osKernelGetTickCount();
    s_qos1_pending[slot].retain      = false;
    s_qos1_pending[slot].payload_len = plen;
    strncpy(s_qos1_pending[slot].topic, topic, sizeof(s_qos1_pending[slot].topic) - 1);
    s_qos1_pending[slot].topic[sizeof(s_qos1_pending[slot].topic) - 1] = '\0';
    memcpy(s_qos1_pending[slot].payload, payload, plen);

    /* 发送 QoS1 PUBLISH */
    size_t pkt_len = mqtt_build_publish(topic, payload, plen, false, 1, pid, false);
    err_t err = mqtt_write(tx_buf, pkt_len);

    if (err != ERR_OK)
    {
        s_qos1_pending[slot].in_use = false;  /* 发送失败，释放槽 */
        osMutexRelease(g_tx_mutex);
        return -2;
    }
    if (boot_result)
        g_boot_result_packet_id = pid;
    osMutexRelease(g_tx_mutex);
    return 0;
}

/* 普通QoS1发布保持原返回语义：0表示已发送，尚未确认。 */
int MqttClient_PublishQos1(const char *topic, const uint8_t *payload, size_t plen)
{
    return mqtt_publish_qos1(topic, payload, plen, false);
}

/* 重复提交同一启动结果只查询在途状态，断线后由调用方以相同身份重发。 */
int MqttClient_PublishUpgradeResult(const uint8_t *payload, size_t plen)
{
    if ((payload == NULL) || (plen == 0U) || (g_topic_ota_status[0] == '\0'))
        return -3;
    return mqtt_publish_qos1(g_topic_ota_status, payload, plen, true);
}

int MqttClient_ReportEvent(const char *event_type, const char *detail)
{
    char json[128];
    int len = snprintf(json, sizeof(json), "{\"type\":\"%s\",\"detail\":\"%s\"}",
                       event_type ? event_type : "",
                       detail ? detail : "");
    if (len <= 0) return -1;
    return MqttClient_Publish(g_topic_events, (const uint8_t *)json, (size_t)len);
}

/* ===== 主任务 ===== */

static bool wait_network_ready(uint32_t timeout_ms)
{
    uint32_t started_ms = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - started_ms) < timeout_ms)
    {
        uint32_t poll_ms = HAL_GetTick();
        if (!mqtt_health_wait(1000U))
            return false;
        int32_t link = m2_ethernet_link_wait(1000U);
        if (!mqtt_health_progress())
            return false;
        if (link == ETH_OK)
        {
            if (netif_default != NULL &&
                netif_is_up(netif_default) &&
                !ip4_addr_isany(netif_ip4_addr(netif_default)))
            {
                return true;
            }
        }
        uint32_t elapsed = (uint32_t)(HAL_GetTick() - poll_ms);
        if (elapsed < 1000U && !mqtt_delay(1000U - elapsed))
            return false;
    }
    return false;
}

/* 关闭发送准入后与发布者互斥删除；失败保留连接，禁止覆盖最后的资源引用。 */
static bool mqtt_connection_delete(void)
{
    g_connected = false;
    if (!mqtt_lock_tx())
        return false;

    if (!mqtt_health_wait(20000U))
    {
        osMutexRelease(g_tx_mutex);
        return false;
    }
    /* 连接内的QoS1身份不可跨重连使用；业务层保留原始数据并重发。 */
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++)
        s_qos1_pending[i].in_use = false;
    g_boot_result_packet_id = 0;
    err_t err = netconn_delete(g_conn);
    if (err == ERR_OK)
        g_conn = NULL;
    bool healthy = mqtt_health_progress();
    osMutexRelease(g_tx_mutex);
    if (err != ERR_OK)
        printf("[MQTT] connection delete failed err=%d\r\n", (int)err);
    return err == ERR_OK && healthy;
}

/* MQTT任务独立登记监督，连接与业务状态机推进后才报告自身进展。 */
void MqttClient_Task(void *argument)
{
    (void)argument;
    g_mqtt_thread = osThreadGetId();
    if (!Monitor_RegisterHealth(APP_MODULE_MQTT, 2000U, 30000U))
        goto health_fail;
    printf("[MQTT] task started, broker=%s:%u cid=%s auth=%s\r\n",
           g_cfg.broker_host, g_cfg.broker_port, g_cfg.client_id,
           g_cfg.username[0] ? "yes" : "no");

    while (1)
    {
        if (g_conn != NULL && !mqtt_connection_delete())
        {
            if (!mqtt_delay(mqtt_backoff_delay(g_backoff_attempt++))) goto health_fail;
            continue;
        }

        /* ---- 1. 等网络就绪 ---- */
        if (!wait_network_ready(60000))
        {
            printf("[MQTT] network not ready, retry\r\n");
            if (!mqtt_delay(5000)) goto health_fail;
            continue;
        }
        printf("[MQTT] network ready, IP=%s\r\n",
               ipaddr_ntoa(netif_ip4_addr(netif_default)));

        /* ---- 2. TCP 连接 broker ---- */
        /* DNS 解析 broker 域名 → IP（同步，走 tcpip 线程；LWIP_DNS=1） */
        printf("[MQTT] resolving %s ...\r\n", g_cfg.broker_host);
        /* 当前DNS两服务器各最多7秒重试，20秒覆盖应答/失败返回。 */
        if (!mqtt_health_wait(20000U)) goto health_fail;
        err_t dnserr = netconn_gethostbyname(g_cfg.broker_host, &g_cfg.broker_ip);
        if (!mqtt_health_progress()) goto health_fail;
        if (dnserr != ERR_OK)
        {
            printf("[MQTT] DNS resolve '%s' failed err=%d\r\n", g_cfg.broker_host, (int)dnserr);
            if (!mqtt_delay(mqtt_backoff_delay(g_backoff_attempt++))) goto health_fail;
            continue;
        }
        g_cfg.broker_ip_valid = true;
        printf("[MQTT] DNS %s -> %s, connecting ...\r\n",
               g_cfg.broker_host, ipaddr_ntoa(&g_cfg.broker_ip));

#if MQTT_USE_BAIDU
        /* 百度云：每次连接重算签名（timestamp 变化） */
        baidu_build_auth();
#endif

        if (!mqtt_health_wait(1000U)) goto health_fail;
        g_conn = netconn_new_with_callback(NETCONN_TCP, mqtt_netconn_event);
        if (!mqtt_health_progress()) goto health_fail;
        if (g_conn == NULL)
        {
            printf("[MQTT] netconn_new failed\r\n");
            if (!mqtt_delay(mqtt_backoff_delay(g_backoff_attempt++))) goto health_fail;
            continue;
        }

        /* ★关键：设 recv 超时 1 秒，主循环才能定期醒来发 PINGREQ 心跳 + 跑补传 pump。
           不设的话 netconn_recv 永久阻塞 → 心跳不发 → keepalive(30s) 超时云端断连。 */
        netconn_set_recvtimeout(g_conn, 1000);
        netconn_set_sendtimeout(g_conn, MQTT_IO_TIMEOUT_MS);

        err_t err = mqtt_connect();
        if (err != ERR_OK)
        {
            printf("[MQTT] TCP connect failed err=%d\r\n", (int)err);
            goto conn_fail;
        }

        /* ---- 3. 发 CONNECT ---- */
        rx_ring_len = 0;
        size_t pkt_len = mqtt_build_connect();
        printf("[MQTT] CONNECT %u bytes, sending...\r\n", (unsigned)pkt_len);
        err = mqtt_write(tx_buf, pkt_len);
        if (err != ERR_OK)
        {
            printf("[MQTT] CONNECT write failed err=%d\r\n", (int)err);
            goto conn_fail;
        }

        /* ---- 4. 等 CONNACK ---- */
        {
            bool got_connack = false;
            uint32_t t0 = osKernelGetTickCount();
            while ((osKernelGetTickCount() - t0) < MQTT_CONNACK_TIMEOUT_MS)
            {
                struct netbuf *nb;
                err = mqtt_receive(&nb);
                if (err == ERR_OK && nb != NULL)
                {
                    void *dp; uint16_t dl;
                    netbuf_data(nb, &dp, &dl);
                    rx_ring_append((const uint8_t *)dp, dl);
                    netbuf_delete(nb);

                    uint8_t mtype, mflags;
                    const uint8_t *mbody;
                    uint32_t mbody_len;
                    size_t flen = rx_ring_take_frame(&mtype, &mflags, &mbody, &mbody_len);
                    if (flen > 0)
                    {
                        if (mtype == 2)  /* CONNACK = 0x20 >> 4 = 2 */
                        {
                            uint8_t rc = (mbody_len >= 2) ? mbody[1] : 0xFF;
                            printf("[MQTT] CONNACK rc=%u (0=accepted 1=bad_proto 2=bad_cid 3=server_unavail 4=bad_auth 5=not_auth)\r\n",
                                   (unsigned)rc);
                            if (rc == 0)
                            {
                                got_connack = true;
                                rx_ring_consume(flen);
                                break;
                            }
                            else
                            {
                                /* ★★原 bug：错误 CONNACK 被静默丢弃，继续等直到超时。
                                   修复：认证失败直接退出，不浪费时间 */
                                printf("[MQTT] CONNECT rejected by broker (rc=%u)\r\n", (unsigned)rc);
                                rx_ring_consume(flen);
                                goto conn_fail;
                            }
                        }
                        printf("[MQTT] unexpected msg type=%u (waiting CONNACK)\r\n", (unsigned)mtype);
                        rx_ring_consume(flen);
                    }
                }
                else
                {
                    /* ★★调试：打印 netconn_recv 返回的错误码，判断是超时还是连接关闭 */
                    if (err != ERR_TIMEOUT)
                        printf("[MQTT] recv err=%d (timeout=%d closed=%d)\r\n",
                               (int)err, (int)ERR_TIMEOUT, (int)ERR_CLSD);
                    if (nb != NULL) netbuf_delete(nb);
                    if (err != ERR_TIMEOUT)
                    {
                        goto conn_fail;
                    }
                }
            }
            if (!got_connack)
            {
                printf("[MQTT] CONNACK timeout (%ums, no MQTT data received)\r\n",
                       (unsigned)MQTT_CONNACK_TIMEOUT_MS);
                goto conn_fail;
            }
        }
        printf("[MQTT] connected!\r\n");

        /* ---- 5. 发 SUBSCRIBE（commands + acks）+ 等 SUBACK ---- */
        pkt_len = mqtt_build_subscribe();
        err = mqtt_write(tx_buf, pkt_len);
        if (err != ERR_OK) goto conn_fail;

        {
            bool got_suback = false;
            bool suback_ok = false;
            uint32_t t0 = osKernelGetTickCount();
            while ((osKernelGetTickCount() - t0) < MQTT_SUBACK_TIMEOUT_MS)
            {
                struct netbuf *nb;
                err = mqtt_receive(&nb);
                if (err == ERR_OK && nb != NULL)
                {
                    void *dp; uint16_t dl;
                    netbuf_data(nb, &dp, &dl);
                    rx_ring_append((const uint8_t *)dp, dl);
                    netbuf_delete(nb);

                    uint8_t mtype, mflags;
                    const uint8_t *mbody;
                    uint32_t mbody_len;
                    size_t flen = rx_ring_take_frame(&mtype, &mflags, &mbody, &mbody_len);
                    if (flen > 0)
                    {
                        if (mtype == 9)   /* SUBACK */
                        {
                            got_suback = true;
                            /* SUBACK: PacketID(2) + return code(s)
                             * 订阅了 2 个 topic，应有 2 个 return code
                             * 0x00=QoS0成功 0x01=QoS1成功 0x80=失败
                             * 设计 5.1: 检查两个 SUBACK 均成功才业务在线 */
                            if (mbody_len >= 4)
                            {
                                if (mbody[2] != 0x80 && mbody[3] != 0x80)
                                {
                                    suback_ok = true;
                                }
                                else
                                {
                                    printf("[MQTT] SUBACK rejected: rc0=0x%02X rc1=0x%02X\r\n",
                                           mbody[2], mbody[3]);
                                }
                            }
                            else if (mbody_len >= 3 && mbody[2] != 0x80)
                            {
                                /* 只有一个 return code（某些 broker） */
                                suback_ok = true;
                            }
                            rx_ring_consume(flen);
                            break;
                        }
                        rx_ring_consume(flen);
                    }
                }
                else if (nb != NULL)
                {
                    netbuf_delete(nb);
                }
                if (err != ERR_OK && err != ERR_TIMEOUT) goto conn_fail;
            }
            if (!got_suback || !suback_ok)
            {
                printf("[MQTT] SUBACK failed/timeout, disconnect\r\n");
                goto conn_fail;
            }
            printf("[MQTT] subscribed commands + acks\r\n");
        }

        /* ---- 6. 上报 online（retain=true）+ boot 事件 ---- */
        g_connected = true;
        g_backoff_attempt = 0;   /* 连接成功，重置退避 */
        {
            const char *online = "online";
            /* online 状态 retain=true（设计 5.1: 状态 retain=true） */
            if (!mqtt_lock_tx()) goto conn_fail;
            size_t plen = mqtt_build_publish(g_topic_status, (const uint8_t *)online, 6, true, 0, 0, false);
            err = mqtt_write(tx_buf, plen);
            osMutexRelease(g_tx_mutex);
            if (err != ERR_OK) goto conn_fail;

            MqttClient_ReportEvent("boot", "gateway started");
            printf("[MQTT] online + boot event published, entering main loop\r\n");
        }

        /* ---- 7. 主循环：收消息 + 心跳 + 补传 ---- */
        {
            uint32_t last_ping = osKernelGetTickCount();
            uint32_t last_replay = 0;

            while (g_connected)
            {
                mqtt_pause_checkpoint();
                struct netbuf *nb;
                err = mqtt_receive(&nb);

                if (err == ERR_TIMEOUT)
                {
                    /* 低频心跳：每 10 秒确认主循环在跑、recv 超时生效 */
                    static uint32_t last_alive = 0;
                    uint32_t now_tick = osKernelGetTickCount();
                    if (now_tick - last_alive >= 10000U)
                    {
                        printf("[MQTT] main loop alive (keepalive=%us)\r\n", (unsigned)g_cfg.keepalive_sec);
                        last_alive = now_tick;
                    }
                    /* 心跳：超过 keepalive 的 80% 没发数据就发 PINGREQ */
                    if ((osKernelGetTickCount() - last_ping) >= (uint32_t)(g_cfg.keepalive_sec * 800))
                    {
                        if (!mqtt_lock_tx()) break;
                        size_t plen = mqtt_build_pingreq();
                        err_t werr = mqtt_write(tx_buf, plen);
                        osMutexRelease(g_tx_mutex);
                        if (werr != ERR_OK)
                        {
                            printf("[MQTT] ping write fail, disconnect\r\n");
                            break;
                        }
                        last_ping = osKernelGetTickCount();
                    }

                    /* 补传 pump */
                    if ((osKernelGetTickCount() - last_replay) >= MQTT_REPLAY_INTERVAL_MS)
                    {
                        mqtt_replay_pump();
                        last_replay = osKernelGetTickCount();
                    }
                    /* QoS1 超时重发检查（设计 5.2: 30s 未收到 PUBACK 重发） */
                    mqtt_qos1_check_resend();
                    continue;
                }

                if (err != ERR_OK || nb == NULL)
                {
                    printf("[MQTT] read err=%d, disconnect\r\n", (int)err);
                    break;
                }

                /* 追加到 rx_ring，循环切帧处理 */
                void *dp; uint16_t dl;
                netbuf_data(nb, &dp, &dl);
                rx_ring_append((const uint8_t *)dp, dl);
                netbuf_delete(nb);

                for (;;)
                {
                    uint8_t mtype, mflags;
                    const uint8_t *mbody;
                    uint32_t mbody_len;
                    size_t flen = rx_ring_take_frame(&mtype, &mflags, &mbody, &mbody_len);
                    if (flen == 0) break;

                    if (mtype == 3)        /* PUBLISH：下行命令 / 业务回执 */
                    {
                        printf("[MQTT] PUBLISH recv flags=0x%02X body=%lu\r\n",
                               (unsigned)mflags, (unsigned long)mbody_len);
                        /* 根据 topic 分发：commands → 命令处理，acks → 回执处理 */
                        if (mbody_len >= 4)
                        {
                            uint16_t tlen = ((uint16_t)mbody[0] << 8) | mbody[1];
                            if ((uint32_t)(2 + tlen) <= mbody_len)
                            {
                                const char *topic = (const char *)&mbody[2];
                                if (tlen == strlen(g_topic_commands) &&
                                    memcmp(topic, g_topic_commands, tlen) == 0)
                                {
                                    mqtt_handle_command(mbody, mbody_len, mflags);
                                }
                                else if (tlen == strlen(g_topic_acks) &&
                                         memcmp(topic, g_topic_acks, tlen) == 0)
                                {
                                    mqtt_handle_ack(mbody, mbody_len);
                                }
                            }
                        }
                    }
                    else if (mtype == 13)  /* PINGRESP */
                    {
                        /* 心跳回复，无需处理 */
                    }
                    else if (mtype == 4)  /* PUBACK: QoS1 PUBLISH 确认 */
                    {
                        if (mbody_len >= 2)
                        {
                            uint16_t pid = ((uint16_t)mbody[0] << 8) | mbody[1];
                            mqtt_handle_puback(pid);
                        }
                    }
                    else
                    {
                        /* 其它类型忽略 */
                    }
                    rx_ring_consume(flen);
                    if (!mqtt_health_progress()) goto health_fail;
                }
            }
        }

        /* ---- 8. 断连清理 ---- */
        (void)mqtt_connection_delete();
        rx_ring_len = 0;
        g_fetch_in_flight = false;
        g_replay_last_seq = 0;
        g_replay_last_tick = 0;
        {
            uint32_t delay = mqtt_backoff_delay(g_backoff_attempt++);
            printf("[MQTT] disconnected, reconnect in %lums (attempt=%lu)\r\n",
                   delay, (unsigned long)(g_backoff_attempt - 1));
            if (!mqtt_delay(delay)) goto health_fail;
        }
        continue;

    conn_fail:
        (void)mqtt_connection_delete();
        rx_ring_len = 0;
        {
            uint32_t delay = mqtt_backoff_delay(g_backoff_attempt++);
            printf("[MQTT] connect failed, retry in %lums (attempt=%lu)\r\n",
                   delay, (unsigned long)(g_backoff_attempt - 1));
            if (!mqtt_delay(delay)) goto health_fail;
        }
    }

health_fail:
    taskENTER_CRITICAL();
    g_pause_acknowledged = false;
    taskEXIT_CRITICAL();
    g_connected = false;
    Monitor_ReportFatal(APP_MODULE_MQTT);
    osThreadExit();
}
