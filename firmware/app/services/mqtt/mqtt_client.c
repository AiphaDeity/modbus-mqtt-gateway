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
 *   7. 补传每次取最多 STORAGE_M2_FETCH_BATCH_MAX 条
 *   8. payload 最大 4096B（M2-03，设计 5.2）
 *   9. 回执 30s 未到则保持原 ID 重发；PUBACK 不回收缓存（设计 5.2）
 *
 * 数据流：
 *   上行(遥测): telemetry_task 采 Modbus -> 带record_id的JSON -> M3提交 -> MQTT统一发送
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
#include "app_readiness.h"
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
#include "storage_identity.h"
#include "storage_seq.h"     /* storage_seq_alloc() */
#include "storage_time.h"   /* storage_get_unix_time() 取 RTC 时间戳做百度云签名 */
#include "lwip/dns.h"        /* dns_gethostbyname 解析云平台域名 */
#include "hmac_sha256.h"     /* 百度云签名 SHA256 */
#if GATEWAY_ENABLE_M2
#include "network_upgrade.h" /* NetworkUpgrade_Notify() OTA 升级通知分流 */
#include "config_task.h"     /* Config_HandleMessage() 配置变更命令分流 */
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

#define MQTT_DEF_BROKER_IP0   192     // 默认broker IP第一段，拼起来192.168.2.45，本地模式回退用
#define MQTT_DEF_BROKER_IP1   168     // 默认broker IP第二段
#define MQTT_DEF_BROKER_IP2   2       // 默认broker IP第三段
#define MQTT_DEF_BROKER_IP3   45      // 默认broker IP第四段
#define MQTT_DEF_BROKER_PORT  1883    // 默认MQTT端口1883，明文TCP不加密
#define MQTT_DEF_CLIENT_ID    "mb2mqtt_001"  // 默认客户端ID，本地mosquitto模式用，百度云用设备key
#define MQTT_DEF_KEEPALIVE    30      // 心跳间隔30秒，超过30秒没发心跳broker判定掉线
#define MQTT_SUBACK_TIMEOUT_MS 2000   // 等SUBACK订阅确认超时2秒
#define MQTT_CONNACK_TIMEOUT_MS 5000  // 等CONNACK连接确认超时5秒，给云端多留处理时间
#define MQTT_REPLAY_INTERVAL_MS 3000  // 补传pump调度间隔3秒，有新数据时才触发
#define MQTT_REPLAY_RESEND_MS  30000  // 业务回执未到重发间隔30秒
#define MQTT_MAX_PAYLOAD       4096   // 最大载荷4096字节，超长报文直接拒绝不截断
#define MQTT_IO_TIMEOUT_MS    10000U  // 收发IO超时10秒，读或写卡住就断
#define MQTT_TX_WAIT_MS       12000U  // 等发送锁最长12秒，拿不到锁就放弃
#define MQTT_HEALTH_MARGIN_MS  1000U  // 健康监控余量1秒，阻塞前预留喂狗时间

/* ===== QoS1 未确认队列（设计 5.2: PUBLISH 带 packet_id → 等 PUBACK → 30s 超时重发）===== */
#define MQTT_QOS1_PENDING_MAX   2     // 同时最多2条QoS1未确认报文，受RAM限制
#define MQTT_RECORD_HEADER_MAX 160U  // 记录头最大160字节，覆盖768B正文+本机身份+两个20位序号的前缀
#define MQTT_QOS1_PAYLOAD_MAX   (RECORD_PAYLOAD_MAX + MQTT_RECORD_HEADER_MAX)  // QoS1载荷上限=记录载荷+头部
/* MQTT_REPLAY_RESEND_MS=30000 复用为 QoS1 重发超时，30秒没PUBACK就重发 */

typedef struct {
    bool        in_use;         // 槽位是否被占用，true表示这条记录正在等PUBACK
    uint16_t    packet_id;      // 报文ID，broker回PUBACK时按这个配对
    uint32_t    send_tick;      // 发送时刻tick，用来算30秒超时重发
    bool        retain;         // 是否retain消息，重发时要还原原标志
    char        topic[96];      // 主题名缓存，重发时要用原主题
    uint8_t     payload[MQTT_QOS1_PAYLOAD_MAX];  // 报文载荷缓存，重发时要用原文
    size_t      payload_len;    // 载荷实际长度
} qos1_pending_t;               // QoS1待确认记录结构体定义结束

// 2条待确认记录放CCMRAM，不占普通SRAM，PUBACK只释放packet_id
static qos1_pending_t s_qos1_pending[MQTT_QOS1_PENDING_MAX] __attribute__((section(".bss.ccmram")));

/* 重连退避表：1/2/4/8/16/32/60秒，超过表长固定用60秒封顶 */
static const uint32_t g_backoff_table[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000};  // 退避毫秒序列
#define BACKOFF_TABLE_LEN  (sizeof(g_backoff_table) / sizeof(g_backoff_table[0]))  // 退避表元素个数7

#define MQTT_TOPIC_STATUS     "status"      // 在线状态子主题，设备和遗嘱都发这
#define MQTT_TOPIC_EVENTS     "events"      // 事件告警子主题，设备主动上报异常
#define MQTT_TOPIC_TELEMETRY  "telemetry"   // 遥测数据子主题，周期采集数据发这
#define MQTT_TOPIC_COMMANDS   "commands"    // 下行命令子主题，设备订阅这个收平台命令
#define MQTT_TOPIC_ACKS       "acks"        // 业务回执子主题，设备订阅这个收平台确认
#define MQTT_TOPIC_CMD_ACK    "cmd_ack"     // 命令执行回执子主题，设备发命令结果
#define MQTT_TOPIC_OTA_STATUS "ota_status"  // OTA升级状态子主题，设备上报升级进度

/* ===== 运行时配置结构体，Init 时从 AppConfig 加载一次 ===== */
typedef struct
{
    char     broker_host[64];    // broker域名或IP，百度云是域名本地是IP
    uint16_t broker_port;        // broker端口，默认1883
    ip_addr_t broker_ip;         // DNS解析后的IP地址，连接时用
    bool     broker_ip_valid;    // DNS解析成功标志，真表示IP可用
    char     client_id[48];      // 客户端ID，百度云=DeviceKey，本地=自定义
    char     username[96];       // 用户名，百度云thingidp@id|key|ts|SHA256较长所以加宽
    char     password[72];       // 密码，百度云=SHA256的64字符hex，本地=明文密码
    uint16_t keepalive_sec;      // 心跳间隔秒数
} mqtt_runtime_cfg_t;            // 运行时配置结构体定义结束

static mqtt_runtime_cfg_t g_cfg;  // 全局运行时配置实例，Init时加载后只读

/* topic 缓冲：Init 时按 client_id 拼好，后面直接用 */
static char g_topic_status[96];     // status主题缓存，拼好的完整主题
static char g_topic_events[96];     // events主题缓存
static char g_topic_telemetry[96];  // telemetry主题缓存
static char g_topic_commands[96];   // commands主题缓存
static char g_topic_acks[96];       // acks主题缓存
static char g_topic_cmd_ack[96];    // cmd_ack主题缓存
static char g_topic_ota_status[96]; // ota_status主题缓存

/* ===== 收发缓冲，放CCMRAM不挤占SRAM里的FreeRTOS堆
 * netconn_write用NETCONN_COPY拷贝到LwIP内部pbuf，tx_buf不需DMA可达，放CCMRAM安全；
 * rx_ring/rx_payload_buf全是CPU处理，同样安全 */
static uint8_t tx_buf[4200]  __attribute__((section(".bss.ccmram")));  // 发送缓冲，组好的报文先放这
static uint8_t rx_ring[4400] __attribute__((section(".bss.ccmram")));  // 接收环缓冲，TCP分段拼包用
static uint16_t rx_ring_len = 0;   // rx_ring当前已有效字节数

/* 命令和回执仅由MQTT任务串行解析，处理返回前消费或拷贝数据，不向异步任务保留指针 */
static char rx_payload_buf[MQTT_MAX_PAYLOAD + 16] __attribute__((section(".bss.ccmram")));  // 命令/回执载荷缓冲

/* ===== 全局状态 ===== */
static struct netconn *g_conn = NULL;      // TCP连接句柄，连上才有值，断线置空
static volatile bool   g_connected = false; // 当前是否已连接broker
static volatile bool   g_connect_done;      // 连接尝试是否结束标志
static osThreadId_t    g_mqtt_thread;       // MQTT任务自己的句柄，喂狗判断用
static volatile bool g_pause_requested;     // 是否请求暂停，升级静默时置位
static bool g_pause_acknowledged;           // 暂停是否已确认

static void mqtt_pause_checkpoint(void);    // 暂停检查点函数声明

/* 发送互斥锁：保护tx_buf和netconn_write，多任务并发发送安全 */
static osMutexId_t     g_tx_mutex;          // 发送锁句柄
static const osMutexAttr_t g_tx_mutex_attr = { .name = "mqtt_tx" };  // 发送锁属性，名字mqtt_tx

/* 下行写寄存器的同步信号量，mb_inject_write执行完释放 */
static osSemaphoreId_t g_write_sem;         // 写命令完成信号量
static mb_errot_t      g_write_err;         // 写命令执行结果错误码

/* 下一个MQTT packet id，SUBSCRIBE和QoS1报文都要用 */
static uint16_t g_next_packet_id = 1;       // 报文ID计数器，从1开始，溢出回1
/* 启动结果只占用原QoS1槽，由发送锁保护，断线只撤销未确认的packet_id */
static uint16_t g_boot_result_packet_id;    // 启动结果报文的packet_id
static bool g_boot_result_acked;            // 启动结果是否已收到PUBACK

/* 重连退避计数器，失败次数，连接成功后清零 */
static uint32_t g_backoff_attempt = 0;      // 连续重连失败次数，查退避表用

/* 补传回执追踪：记录上次发送的最大seq和时间，用于30秒重发判定 */
static uint64_t g_replay_last_seq = 0;      // 上次补传发送的最大序号
static uint32_t g_replay_last_tick = 0;     // 上次补传发送的时间戳
// 保留原批次到业务确认，位图仅标记本批已发送/已确认项，不增加记录副本
static int mqtt_publish_qos1(const char *topic, const uint8_t *payload, size_t plen,
                             bool boot_result, uint64_t record_seq);  // QoS1发布内部函数声明
static uint8_t g_replay_sent_mask;          // 本批已发送项的位图
static uint8_t g_replay_ack_mask;           // 本批已确认项的位图
_Static_assert(STORAGE_M2_FETCH_BATCH_MAX <= 8U, "Replay mask capacity");  // 编译期断言批次不超过8条，位图才装得下
static storage_fetch_resp_t g_fetch_resp __attribute__((section(".bss.ccmram")));  // 补传取数应答大缓冲，放CCMRAM

/* 只由MQTT任务更新自身进展；其他发布调用方自行监督等待。 */
static bool mqtt_health_progress(void)   // 健康监控：喂狗，上报MQTT任务进展正常
{
    if (osThreadGetId() != g_mqtt_thread)  // 当前不是MQTT任务自己调用，而是别的发布任务调用
        return Monitor_UpgradeProgress();  // 走升级监控的进展上报路径
    if (Monitor_ReportProgress(APP_MODULE_MQTT, true))  // 向监控模块报告MQTT模块进展正常
        return true;  // 上报成功，返回真
    Monitor_ReportFatal(APP_MODULE_MQTT);  // 上报失败说明健康异常，标记致命错误
    return false;  // 返回假，让调用方中断当前操作
}

/* 声明一次阻塞操作的期限，返回后必须结束本次等待。 */
static bool mqtt_health_wait(uint32_t timeout_ms)  // 阻塞前登记：告诉监控模块我马上要等timeout_ms毫秒
{
    if (osThreadGetId() != g_mqtt_thread)  // 不是MQTT任务调用，走升级监控等待
        return Monitor_UpgradeWait(timeout_ms);  // 交给升级监控登记等待
    if (Monitor_BeginWait(APP_MODULE_MQTT, timeout_ms + MQTT_HEALTH_MARGIN_MS))  // 登记等待时长，多留1秒余量
        return true;  // 登记成功返回真
    Monitor_ReportFatal(APP_MODULE_MQTT);  // 登记失败标记致命错误
    return false;  // 返回假
}

/* 保留原退避总时长，每秒核对单调期限，允许监督发现等待中的停滞。 */
static bool mqtt_delay(uint32_t delay_ms)  // 分段延时：把长延时切成1秒一段，每段都喂狗
{
    uint32_t started_ms = HAL_GetTick();  // 记录开始时间戳
    for (;;)  // 死循环，直到延时结束或健康失败才返回
    {
        mqtt_pause_checkpoint();  // 每段先检查暂停请求，升级静默要能中断延时
        uint32_t elapsed = (uint32_t)(HAL_GetTick() - started_ms);  // 算已经过了多久
        if (elapsed >= delay_ms)  // 已经等够目标时长
            return true;  // 延时完成返回真
        uint32_t remaining = delay_ms - elapsed;  // 还差多少毫秒
        uint32_t step_ms = remaining < 1000U ? remaining : 1000U;  // 每段最多1秒，不足1秒按剩余算
        if (!mqtt_health_wait(step_ms) || osDelay(pdMS_TO_TICKS(step_ms)) != osOK)  // 登记等待再实际延时，失败就返回
            return false;  // 登记失败或延时失败返回假
        if (!mqtt_health_progress())  // 每段延时后喂狗
            return false;  // 喂狗失败返回假
    }
}

/* 有界获取现有发送锁，失败不继续访问共享发送缓冲。 */
static bool mqtt_lock_tx(void)  // 拿发送锁：保护tx_buf和netconn_write不被多任务同时写
{
    if (!mqtt_health_wait(MQTT_TX_WAIT_MS))  // 先登记最多等12秒
        return false;  // 登记失败返回假
    osStatus_t status = osMutexAcquire(g_tx_mutex, pdMS_TO_TICKS(MQTT_TX_WAIT_MS));  // 有界获取互斥锁，最多等12秒
    if (status != osOK && status != osErrorTimeout)  // 既没拿到锁也不是超时，说明是异常错误
    {
        if (osThreadGetId() == g_mqtt_thread)  // 是MQTT任务自己拿锁出错
            Monitor_ReportFatal(APP_MODULE_MQTT);  // 标记致命错误
        return false;  // 返回假
    }
    bool healthy = mqtt_health_progress();  // 拿锁后喂狗
    if (status == osOK && healthy)  // 拿到锁且健康正常
        return true;  // 返回真，锁已持有
    if (status == osOK)  // 拿到锁但健康异常
        osMutexRelease(g_tx_mutex);  // 释放锁再返回，避免死锁
    return false;  // 返回假
}

/* 发送完成或失败均结束等待；发送失败关闭准入，避免后续帧接在半包之后。 */
static err_t mqtt_write(const void *data, size_t size)  // 底层TCP发送：把data的size字节发出去
{
    if (!mqtt_health_wait(MQTT_IO_TIMEOUT_MS))  // 登记10秒发送等待
    {
        g_connected = false;  // 登记失败置连接断开
        return ERR_ABRT;  // 返回中止错误
    }
    err_t err = netconn_write(g_conn, data, size, NETCONN_COPY);  // 真正调LwIP发送，NETCONN_COPY表示拷到内部pbuf
    if (!mqtt_health_progress())  // 发送后喂狗
        err = ERR_ABRT;  // 喂狗失败改错误码为中止
    if (err != ERR_OK)  // 发送没成功
        g_connected = false;  // 置连接断开，后面会触发重连
    return err;  // 返回发送结果
}

/* LwIP回调只记录连接结果到达，不替MQTT任务报告健康。 */
static void mqtt_netconn_event(struct netconn *conn, enum netconn_evt event, u16_t size)  // LwIP连接事件回调
{
    (void)size;  // 忽略size参数，消除未使用警告
    if (conn == g_conn && (event == NETCONN_EVT_SENDPLUS || event == NETCONN_EVT_ERROR))  // 本连接发生可发送或错误事件
        g_connect_done = true;  // 标记连接结果已到达，主循环据此结束等待
}

/* 非阻塞发起连接，最多10秒；只在任务中检查结果并推进原连接状态机。 */
static err_t mqtt_connect(void)  // 非阻塞TCP连接：发起连接并最多等10秒
{
    g_connect_done = false;  // 清连接完成标志
    netconn_set_nonblocking(g_conn, 1);  // 先设为非阻塞，连接不会卡住任务
    if (!mqtt_health_wait(1000U))  // 登记1秒等待
    {
        netconn_set_nonblocking(g_conn, 0);  // 恢复阻塞模式
        return ERR_TIMEOUT;  // 登记失败返回超时
    }
    uint32_t started_ms = HAL_GetTick();  // 记录开始时间
    err_t err = netconn_connect(g_conn, &g_cfg.broker_ip, g_cfg.broker_port);  // 发起连接，非阻塞下返回INPROGRESS
    if (!mqtt_health_progress())  // 发起后喂狗
        err = ERR_TIMEOUT;  // 喂狗失败改超时
    while (err == ERR_INPROGRESS)  // 连接还在进行中，循环等结果
    {
        if (g_connect_done)  // LwIP回调通知结果到了
        {
            err = netconn_err(g_conn);  // 取最终连接结果
            break;  // 退出循环
        }
        uint32_t elapsed = (uint32_t)(HAL_GetTick() - started_ms);  // 算已过多久
        if (elapsed >= MQTT_IO_TIMEOUT_MS)  // 超过10秒还没连上
        {
            err = ERR_TIMEOUT;  // 置超时
            break;  // 退出循环
        }
        uint32_t remaining = MQTT_IO_TIMEOUT_MS - elapsed;  // 还差多少到超时
        if (!mqtt_delay(remaining < 100U ? remaining : 100U))  // 分段等一小段，每段最多100毫秒且喂狗
        {
            err = ERR_TIMEOUT;  // 延时失败置超时
            break;  // 退出循环
        }
    }
    netconn_set_nonblocking(g_conn, 0);  // 连接结束恢复阻塞模式
    return err;  // 返回连接结果
}

/* ===== 配置加载 ===== */
static void mqtt_config_load(void)  // 加载MQTT运行时配置：broker地址、端口、客户端ID、用户名密码、心跳
{
#if MQTT_USE_BAIDU  // 百度云模式编译这段
    /* ===== 百度云模式：broker 域名 + SHA256 签名 ===== */
    snprintf(g_cfg.broker_host, sizeof(g_cfg.broker_host),  // 拼broker域名
             "%s.iot.%s.baidubce.com", BAIDU_IOTCORE_ID, BAIDU_REGION);  // 格式：实例ID.iot.地域.baidubce.com
    g_cfg.broker_port = 1883;  // 百度云明文端口1883
    g_cfg.broker_ip_valid = false;     // 连接时才DNS解析域名，先置无效
    g_cfg.keepalive_sec = MQTT_DEF_KEEPALIVE;  // 心跳30秒
#else  // 本地mosquitto模式编译这段
    /* ===== 本地 mosquitto 模式：IP/域名 + 明文认证 ===== */
    const app_config_t *cfg = AppConfig_Get();  // 拿全局配置只读指针
    if (cfg != NULL && cfg->magic == CONFIG_MAGIC && cfg->mq_broker_host[0] != '\0')  // 配置有效且broker非空
    {
        strncpy(g_cfg.broker_host, cfg->mq_broker_host, sizeof(g_cfg.broker_host)-1);  // 拷贝broker地址
        g_cfg.broker_host[sizeof(g_cfg.broker_host)-1] = '\0';  // 强制结尾符防越界
        g_cfg.broker_port = cfg->mq_broker_port ? cfg->mq_broker_port : MQTT_DEF_BROKER_PORT;  // 端口，没配用默认1883
        strncpy(g_cfg.client_id, cfg->mq_client_id, sizeof(g_cfg.client_id)-1);  // 拷贝客户端ID
        g_cfg.client_id[sizeof(g_cfg.client_id)-1] = '\0';  // 强制结尾符
        strncpy(g_cfg.username, cfg->mq_username, sizeof(g_cfg.username)-1);  // 拷贝用户名
        g_cfg.username[sizeof(g_cfg.username)-1] = '\0';  // 强制结尾符
        strncpy(g_cfg.password, cfg->mq_password, sizeof(g_cfg.password)-1);  // 拷贝密码
        g_cfg.password[sizeof(g_cfg.password)-1] = '\0';  // 强制结尾符
        g_cfg.keepalive_sec = 30;  // 心跳30秒
        g_cfg.broker_ip_valid = false;   // 统一走DNS解析，host可能是域名或IP
        return;  // 配置加载完直接返回
    }
    /* 配置无效，回退默认值 */
    strncpy(g_cfg.broker_host, "192.168.2.45", sizeof(g_cfg.broker_host)-1);  // 回退默认broker地址192.168.2.45
    g_cfg.broker_host[sizeof(g_cfg.broker_host)-1] = '\0';  // 强制结尾符
    g_cfg.broker_port  = MQTT_DEF_BROKER_PORT;  // 回退默认端口1883
    strncpy(g_cfg.client_id, MQTT_DEF_CLIENT_ID, sizeof(g_cfg.client_id)-1);  // 回退默认客户端ID
    g_cfg.client_id[sizeof(g_cfg.client_id)-1] = '\0';  // 强制结尾符
    g_cfg.username[0] = '\0';  // 回退空用户名
    g_cfg.password[0] = '\0';  // 回退空密码
    g_cfg.keepalive_sec = MQTT_DEF_KEEPALIVE;  // 回退心跳30秒
    g_cfg.broker_ip_valid = false;  // 无效IP
#endif  // 条件编译结束
}

#if MQTT_USE_BAIDU  // 百度云模式才编译认证函数

/*
 * 每次连接前按DeviceKey和DeviceSecret生成百度MQTT认证参数。
 * clientId=DeviceKey；username=thingidp@{IoTCoreId}|{DeviceKey}|0|SHA256。
 * password为SHA256(DeviceKey + "&0&SHA256" + DeviceSecret)的小写hex。
 */
static void baidu_build_auth(void)  // 百度云认证：生成用户名和密码，每次连接前重算
{
    const char *iotcore_id = BAIDU_IOTCORE_ID;  // IoT实例ID，宏里是aeklwww
    const char *dk = BAIDU_DEVICE_KEY;  // 设备key，宏里是stm32_gw_01
    const char *ds = BAIDU_DEVICE_SECRET;  // 设备密钥，宏里是占位符

    /* 当前MQTT认证使用0时间戳；不能作为HTTPS证书校验的时间来源。 */
    uint32_t ts = 0;  // 时间戳传0，表示百度云不校验时间，板子没RTC只能传0

    /* username = thingidp@{IoTCoreId}|{DeviceKey}|{timestamp}|SHA256 */
    snprintf(g_cfg.username, sizeof(g_cfg.username),  // 拼用户名，格式是百度云规定死的
             "thingidp@%s|%s|%lu|SHA256",  // thingidp@实例ID|设备key|时间戳|算法
             iotcore_id, dk, (unsigned long)ts);

    /* 加密字符串 = {DeviceKey}&{timestamp}&SHA256{DeviceSecret} */
    char sign_str[128];  // 待签名字符串临时数组
    int n = snprintf(sign_str, sizeof(sign_str),  // 拼待签名串
                     "%s&%lu&SHA256%s", dk, (unsigned long)ts, ds);  // 设备key&时间戳&SHA256设备密钥

    /* password = SHA256(加密字符串) 的 hex 小写 */
    sha256_hex((const uint8_t *)sign_str, (size_t)n, g_cfg.password);  // 算SHA256，64字符hex存到密码

    /* clientId = DeviceKey */
    strncpy(g_cfg.client_id, dk, sizeof(g_cfg.client_id)-1);  // 客户端ID就是设备key
    g_cfg.client_id[sizeof(g_cfg.client_id)-1] = '\0';  // 强制结尾符

    printf("[MQTT] Baidu auth: cid=%s ts=%lu\r\n",  // 打印认证信息调试
           g_cfg.client_id, (unsigned long)ts);
}
#endif  // 百度云认证结束

/* 对外：返回遥测主题（telemetry_task 用，替代硬编码 topic，切云平台自动适配） */
const char *MqttClient_TopicTelemetry(void)  // 对外接口：返回遥测主题
{
    return g_topic_telemetry;  // 返回拼好的telemetry完整主题
}

const char *MqttClient_TopicOtaStatus(void)  // 对外接口：返回OTA状态主题
{
    return g_topic_ota_status;  // 返回拼好的ota_status完整主题
}

/* 拼所有主题。
 * 百度云 IoT Core：$iot/{DeviceKey}/user/{子名}（官方固定前缀，详见 IoT Core 文档）
 * 本地 mosquitto：{client_id}/{子名} */
static void mqtt_topics_build(void)  // 拼7个完整MQTT主题到全局数组，Init时拼一次后面直接用
{
#if MQTT_USE_BAIDU  // 百度云模式
    /* 百度云：$iot/{DeviceKey}/user/{子名}。g_cfg.client_id 在 baidu_build_auth 里已设为 DeviceKey */
    const char *dk = g_cfg.client_id;  // dk就是设备key，简化变量名
    snprintf(g_topic_status,    sizeof(g_topic_status),    "$iot/%s/user/%s", dk, MQTT_TOPIC_STATUS);  // 拼status主题
    snprintf(g_topic_events,    sizeof(g_topic_events),    "$iot/%s/user/%s", dk, MQTT_TOPIC_EVENTS);  // 拼events主题
    snprintf(g_topic_telemetry, sizeof(g_topic_telemetry), "$iot/%s/user/%s", dk, MQTT_TOPIC_TELEMETRY);  // 拼telemetry主题
    snprintf(g_topic_commands,  sizeof(g_topic_commands),  "$iot/%s/user/%s", dk, MQTT_TOPIC_COMMANDS);  // 拼commands主题
    snprintf(g_topic_acks,      sizeof(g_topic_acks),      "$iot/%s/user/%s", dk, MQTT_TOPIC_ACKS);  // 拼acks主题
    snprintf(g_topic_cmd_ack,   sizeof(g_topic_cmd_ack),   "$iot/%s/user/%s", dk, MQTT_TOPIC_CMD_ACK);  // 拼cmd_ack主题
    snprintf(g_topic_ota_status,sizeof(g_topic_ota_status),"$iot/%s/user/%s", dk, MQTT_TOPIC_OTA_STATUS);  // 拼ota_status主题
#else  // 本地mosquitto模式
    snprintf(g_topic_status,    sizeof(g_topic_status),    "%s/%s", g_cfg.client_id, MQTT_TOPIC_STATUS);  // 本地拼status：客户端ID/子名
    snprintf(g_topic_events,    sizeof(g_topic_events),    "%s/%s", g_cfg.client_id, MQTT_TOPIC_EVENTS);  // 本地拼events
    snprintf(g_topic_telemetry, sizeof(g_topic_telemetry), "%s/%s", g_cfg.client_id, MQTT_TOPIC_TELEMETRY);  // 本地拼telemetry
    snprintf(g_topic_commands,  sizeof(g_topic_commands),  "%s/%s", g_cfg.client_id, MQTT_TOPIC_COMMANDS);  // 本地拼commands
    snprintf(g_topic_acks,      sizeof(g_topic_acks),      "%s/%s", g_cfg.client_id, MQTT_TOPIC_ACKS);  // 本地拼acks
    snprintf(g_topic_cmd_ack,   sizeof(g_topic_cmd_ack),   "%s/%s", g_cfg.client_id, MQTT_TOPIC_CMD_ACK);  // 本地拼cmd_ack
    snprintf(g_topic_ota_status,sizeof(g_topic_ota_status),"%s/%s", g_cfg.client_id, MQTT_TOPIC_OTA_STATUS);  // 本地拼ota_status
    const app_config_t *config = AppConfig_Get();  // 拿全局配置，看有没有自定义主题
    if (config != NULL)  // 配置有效
    {
        snprintf(g_topic_telemetry, sizeof(g_topic_telemetry), "%s", config->mq_pub_topic);  // 发布主题用配置里的值
        snprintf(g_topic_commands, sizeof(g_topic_commands), "%s", config->mq_sub_topic);  // 订阅主题用配置里的值
    }
#endif  // 条件编译结束
}

/* ===== MQTT 变长 remaining length 编码/解码 ===== */

static int mqtt_encode_remaining_length(uint8_t *buf, uint32_t value)  // 变长编码：把剩余长度写成MQTT变长格式，返回字节数
{
    int n = 0;  // 已写字节数
    do {  // do-while先执行一次，至少写1字节
        uint8_t b = value & 0x7F;  // 取低7位存数据
        value >>= 7;  // 右移7位，准备处理下一组
        if (value > 0) b |= 0x80;  // 还有剩余数据，bit7置1标记后面还有字节
        buf[n++] = b;  // 写1字节，计数加1
    } while (value > 0 && n < 4);  // 还有数据且最多4字节就继续
    return n;  // 返回编码占了几字节
}

static int mqtt_decode_remaining_length(const uint8_t *data, uint16_t avail, uint32_t *out_value, int *out_bytes)  // 变长解码：还原剩余长度数值
{
    uint32_t value = 0;  // 累加结果
    int multiplier = 1;  // 权重，第一字节乘1，第二字节乘128
    int i = 0;  // 已读字节数
    while (i < avail && i < 4)  // 有数据且最多4字节
    {
        uint8_t b = data[i];  // 读当前字节
        value += (uint32_t)(b & 0x7F) * multiplier;  // 低7位乘权重累加
        i++;  // 偏移加1
        if ((b & 0x80) == 0)  // bit7=0说明是最后一字节
        {
            *out_value = value;  // 输出最终长度
            *out_bytes = i;  // 输出编码占了几字节
            return 0;  // 成功返回0
        }
        multiplier *= 128;  // 权重乘128，读下一字节
    }
    return -1;  // 数据不完整或非法，返回-1
}

/* ===== 退避计算（设计 5.1: 1/2/4/8/16/32/60s + 20% 抖动） ===== */
static uint32_t mqtt_backoff_delay(uint32_t attempt)  // 重连退避：按失败次数查表算等待毫秒数，带随机抖动
{
    uint32_t idx = attempt;  // 失败次数当表下标
    if (idx >= BACKOFF_TABLE_LEN) idx = BACKOFF_TABLE_LEN - 1;  // 超过表长用最大60秒
    uint32_t base = g_backoff_table[idx];  // 查基础等待时间
    uint32_t jitter = base / 5;   // 抖动范围是基础的20%
    if (jitter > 0) jitter = (uint32_t)rand() % jitter;  // 生成0到jitter的随机数
    return base + jitter;  // 基础加随机抖动，让设备重连时间错开
}

/* ===== MQTT 报文构造（写入 tx_buf，返回总长度） ===== */

/**
 * 组 CONNECT 报文（带遗嘱 LWT + 用户名/密码认证）
 * 设计 5.1: CleanSession=1, Keepalive, LWT retain=true, 认证
 */
static size_t mqtt_build_connect(void)  // 组CONNECT连接报文，返回报文总长度
{
    uint8_t vh[512];  // 临时数组拼可变头和载荷，512字节足够
    size_t n = 0;  // 临时数组写入偏移

    /* 可变头 */
    vh[n++] = 0x00; vh[n++] = 0x04;  // 协议名长度2字节大端，值是4
    vh[n++] = 'M'; vh[n++] = 'Q'; vh[n++] = 'T'; vh[n++] = 'T';  // 协议名"MQTT"四个字符
    vh[n++] = 0x04;   // 协议级别4，对应MQTT 3.1.1版本

    /* Connect Flags 连接标志字节：
     *   bit0 reserved(0) bit1 CleanSession(1) bit2 WillFlag(1)
     *   bit3-4 WillQoS(0) bit5 WillRetain(1)
     *   bit6 PasswordFlag bit7 UserNameFlag
     * 基础: 0x02(Clean) | 0x04(WillFlag) | 0x20(WillRetain) = 0x26
     * 有认证时: | 0x80(UserName) | 0x40(Password) = 0xE6 */
    bool has_auth = (g_cfg.username[0] != '\0');  // 用户名非空说明需要认证
    uint8_t flags = 0x02 | 0x04 | 0x20;   // 基础标志：CleanSession开+带遗嘱+遗嘱retain
    if (has_auth) flags |= 0x80 | 0x40;    // 有认证再加用户名标志和密码标志
    vh[n++] = flags;  // 标志字节写入可变头

    vh[n++] = (uint8_t)((g_cfg.keepalive_sec >> 8) & 0xFF);  // 心跳间隔高字节
    vh[n++] = (uint8_t)(g_cfg.keepalive_sec & 0xFF);  // 心跳间隔低字节，2字节大端

    /* 载荷：ClientID */
    size_t cidlen = strlen(g_cfg.client_id);  // 算客户端ID长度
    vh[n++] = (uint8_t)((cidlen >> 8) & 0xFF);  // 长度高字节
    vh[n++] = (uint8_t)(cidlen & 0xFF);  // 长度低字节
    memcpy(&vh[n], g_cfg.client_id, cidlen);  // 拷贝客户端ID字符串
    n += cidlen;  // 偏移加长度

    /* 载荷：遗嘱主题（status topic，retain=true） */
    const char *will_topic = g_topic_status;  // 遗嘱主题就是status主题
    size_t wtlen = strlen(will_topic);  // 算遗嘱主题长度
    vh[n++] = (uint8_t)((wtlen >> 8) & 0xFF);  // 长度高字节
    vh[n++] = (uint8_t)(wtlen & 0xFF);  // 长度低字节
    memcpy(&vh[n], will_topic, wtlen);  // 拷贝遗嘱主题
    n += wtlen;  // 偏移加长度

    /* 载荷：遗嘱消息 */
    const char *will_msg = "offline";  // 遗嘱消息固定offline，掉线broker代发
    size_t wmlen = strlen(will_msg);  // 算遗嘱消息长度
    vh[n++] = (uint8_t)((wmlen >> 8) & 0xFF);  // 长度高字节
    vh[n++] = (uint8_t)(wmlen & 0xFF);  // 长度低字节
    memcpy(&vh[n], will_msg, wmlen);  // 拷贝遗嘱消息
    n += wmlen;  // 偏移加长度

    /* 载荷：用户名 + 密码（仅有认证时） */
    if (has_auth)  // 有认证才写用户名密码
    {
        size_t ulen = strlen(g_cfg.username);  // 算用户名长度
        vh[n++] = (uint8_t)((ulen >> 8) & 0xFF);  // 长度高字节
        vh[n++] = (uint8_t)(ulen & 0xFF);  // 长度低字节
        memcpy(&vh[n], g_cfg.username, ulen);  // 拷贝用户名
        n += ulen;  // 偏移加长度

        size_t plen = strlen(g_cfg.password);  // 算密码长度
        vh[n++] = (uint8_t)((plen >> 8) & 0xFF);  // 长度高字节
        vh[n++] = (uint8_t)(plen & 0xFF);  // 长度低字节
        memcpy(&vh[n], g_cfg.password, plen);  // 拷贝密码
        n += plen;  // 偏移加长度
    }

    /* 固定头 */
    size_t off = 0;  // 发送缓冲偏移
    tx_buf[off++] = 0x10;   // 固定头第一字节，高4位1=CONNECT
    off += mqtt_encode_remaining_length(&tx_buf[off], (uint32_t)n);  // 编码剩余长度写入固定头
    memcpy(&tx_buf[off], vh, n);  // 临时数组整体拷到发送缓冲
    return off + n;  // 固定头+可变头+载荷，返回总长度
}

/**
 * 组 SUBSCRIBE 报文（订阅 commands + acks 两个 topic，QoS0）
 * 设计 5.1: 连接成功后订阅 commands 与 acks，检查两个 SUBACK 均成功才业务在线
 * 一个 SUBSCRIBE 报文可含多个 topic filter，SUBACK 回对应数量的 return code
 */
static size_t mqtt_build_subscribe(void)  // 组SUBSCRIBE订阅报文，一次订阅commands和acks两个主题
{
    uint8_t vh[160];  // 临时数组拼可变头，160字节足够
    size_t n = 0;  // 临时数组偏移

    uint16_t pid = g_next_packet_id++;  // 取当前报文ID再自增
    if (g_next_packet_id == 0) g_next_packet_id = 1;  // 16位溢出回1，ID不能为0
    vh[n++] = (uint8_t)((pid >> 8) & 0xFF);  // 报文ID高字节
    vh[n++] = (uint8_t)(pid & 0xFF);  // 报文ID低字节，2字节大端

    /* topic 1: commands */
    size_t tlen1 = strlen(g_topic_commands);  // 算commands主题长度
    vh[n++] = (uint8_t)((tlen1 >> 8) & 0xFF);  // 长度高字节
    vh[n++] = (uint8_t)(tlen1 & 0xFF);  // 长度低字节
    memcpy(&vh[n], g_topic_commands, tlen1);  // 拷贝commands主题
    n += tlen1;  // 偏移加长度
    vh[n++] = 0x00;   // 订阅QoS0，最多发一次不确认

    /* topic 2: acks */
    size_t tlen2 = strlen(g_topic_acks);  // 算acks主题长度
    vh[n++] = (uint8_t)((tlen2 >> 8) & 0xFF);  // 长度高字节
    vh[n++] = (uint8_t)(tlen2 & 0xFF);  // 长度低字节
    memcpy(&vh[n], g_topic_acks, tlen2);  // 拷贝acks主题
    n += tlen2;  // 偏移加长度
    vh[n++] = 0x00;   // 订阅QoS0

    size_t off = 0;  // 发送缓冲偏移
    tx_buf[off++] = 0x82;   // 固定头第一字节，高4位8=SUBSCRIBE，低4位固定0010
    off += mqtt_encode_remaining_length(&tx_buf[off], (uint32_t)n);  // 编码剩余长度
    memcpy(&tx_buf[off], vh, n);  // 临时数组拷到发送缓冲
    return off + n;  // 返回报文总长度
}

/**
 * 组 PUBLISH 报文（支持 QoS0/QoS1，DUP flag，retain 可选）
 * 设计 5.1: 状态 retain=true，其余 retain=false
 * 设计 5.2: OTA状态/补传用 QoS1（带 packet_id + 等 PUBACK + 30s 重发）
 */
static size_t mqtt_build_publish(const char *topic, const uint8_t *payload, size_t plen,
                                 bool retain, uint8_t qos, uint16_t packet_id, bool dup)  // 组PUBLISH发布报文，7个参数
{
    size_t tlen = strlen(topic);  // 算主题长度
    size_t remaining = 2 + tlen + plen;  // 剩余长度=2字节主题长度+主题+载荷
    if (qos > 0) remaining += 2;  // QoS1多2字节报文ID

    size_t off = 0;  // 发送缓冲偏移
    uint8_t header = 0x30 | (retain ? 0x01 : 0x00);  // 头字节高4位3=PUBLISH，retain时bit0置1
    if (qos == 1) header |= 0x02;        // QoS1时bit1置1
    if (dup)      header |= 0x08;        // DUP重发时bit3置1
    tx_buf[off++] = header;  // 固定头第一字节写入
    off += mqtt_encode_remaining_length(&tx_buf[off], (uint32_t)remaining);  // 编码剩余长度
    tx_buf[off++] = (uint8_t)((tlen >> 8) & 0xFF);  // 主题长度高字节
    tx_buf[off++] = (uint8_t)(tlen & 0xFF);  // 主题长度低字节
    memcpy(&tx_buf[off], topic, tlen);  // 拷贝主题字符串
    off += tlen;  // 偏移加主题长度
    if (qos > 0)  // QoS1才写报文ID
    {
        tx_buf[off++] = (uint8_t)((packet_id >> 8) & 0xFF);  // 报文ID高字节
        tx_buf[off++] = (uint8_t)(packet_id & 0xFF);  // 报文ID低字节
    }
    memcpy(&tx_buf[off], payload, plen);  // 拷贝载荷
    off += plen;  // 偏移加载荷长度
    return off;  // 返回报文总长度
}

/* 组 PINGREQ（固定头 0xC0 0x00） */
static size_t mqtt_build_pingreq(void)  // 组心跳请求报文，固定2字节没有载荷
{
    tx_buf[0] = 0xC0;  // 固定头第一字节，高4位C=PINGREQ
    tx_buf[1] = 0x00;  // 剩余长度0，没有可变头和载荷
    return 2;  // 报文总长度就是2字节
}

/* ===== 收包帧重组 ===== */

// 完整接收netbuf链；越界或读失败时断连，不将半帧尾部当作下一条命令。
static bool rx_ring_append(struct netbuf *buffer)  // TCP收包拼包：把netbuf链的数据追加到接收环缓冲
{
    netbuf_first(buffer);  // 定位到netbuf链第一段
    do  // do-while至少处理一段
    {
        void *data = NULL;  // 当前段数据指针
        uint16_t len = 0U;  // 当前段数据长度
        if (netbuf_data(buffer, &data, &len) != ERR_OK ||  // 取当前段数据失败
            (len != 0U && data == NULL) || len > sizeof(rx_ring) - rx_ring_len)  // 指针空或放不下
        {
            printf("[MQTT] invalid receive chain or ring overflow\r\n");  // 打印链非法或缓冲溢出
            return false;  // 返回假，让调用方断连
        }
        if (len != 0U) memcpy(rx_ring + rx_ring_len, data, len);  // 非空段拷到缓冲尾部
        rx_ring_len += len;  // 更新缓冲有效长度
    } while (netbuf_next(buffer) >= 0);  // 还有下一段就继续
    return true;  // 全部拼完返回真
}

static int rx_ring_take_frame(uint8_t *out_type, uint8_t *out_flags,
                                 const uint8_t **out_body, uint32_t *out_body_len)  // 从缓冲取一个完整MQTT帧
{
    if (rx_ring_len < 2) return 0;  // 连2字节固定头都不够，没有完整帧

    uint32_t remaining = 0;  // 解码后的剩余长度
    int rl_bytes = 0;  // 长度编码占了几字节
    if (mqtt_decode_remaining_length(&rx_ring[1], rx_ring_len - 1, &remaining, &rl_bytes) != 0)  // 解码剩余长度失败
        return rx_ring_len >= 5U ? -1 : 0;  // 数据够5字节还解不出来是坏帧返回-1，否则是半包返回0

    size_t frame_total = 1 + rl_bytes + remaining;  // 整帧总长=1固定头+长度字节+剩余载荷
    /* 超长帧拒绝：单帧总长超过 ring 容量，永远装不下，清空丢弃不截断等待 */
    if (frame_total > sizeof(rx_ring))  // 单帧比整个缓冲还大
    {
        printf("[MQTT] drop oversize frame total=%u > ring %u (rejected)\r\n",  // 打印超大帧丢弃
               (unsigned)frame_total, (unsigned)sizeof(rx_ring));
        return -1;  // 返回-1表示坏帧
    }
    if (rx_ring_len < frame_total) return 0;  // 缓冲还不够一帧，等下一段

    *out_type  = (rx_ring[0] >> 4) & 0x0F;  // 高4位是报文类型
    *out_flags = rx_ring[0] & 0x0F;  // 低4位是标志位
    *out_body  = &rx_ring[1 + rl_bytes];  // 载荷起始位置
    *out_body_len = remaining;  // 载荷长度
    return (int)frame_total;  // 返回整帧总长
}

static void rx_ring_consume(size_t frame_total)  // 消费掉已处理的帧，后面的数据搬回开头
{
    if (frame_total >= rx_ring_len)  // 这帧长度大于等于缓冲总长，刚好读完
    {
        rx_ring_len = 0;  // 直接清空缓冲
    }
    else  // 后面还有数据
    {
        memmove(rx_ring, rx_ring + frame_total, rx_ring_len - frame_total);  // 剩余数据搬到开头，memmove防重叠
        rx_ring_len -= frame_total;  // 总长度减去已处理帧
    }
}

// 优先处理已缓存完整帧（buffer为NULL），否则最多接收1秒，保留未完整的帧。
static err_t mqtt_receive(struct netbuf **buffer)  // 收一个完整MQTT帧，优先用缓存，否则recv
{
    *buffer = NULL;  // 先清输出缓冲指针
    uint8_t type, flags;  // 报文类型和标志
    const uint8_t *body;  // 载荷指针
    uint32_t body_len;  // 载荷长度
    int frame = rx_ring_take_frame(&type, &flags, &body, &body_len);  // 先看缓冲里有没有完整帧
    if (frame < 0) return ERR_VAL;  // 坏帧返回非法值
    if (frame > 0) return ERR_OK;  // 缓存里有完整帧，返回OK，调用方自己处理
    if (!mqtt_health_wait(1000U)) return ERR_ABRT;  // 登记1秒接收等待
    err_t err = netconn_recv(g_conn, buffer);  // 真正接收，最多等1秒
    if (!mqtt_health_progress())  // 接收后喂狗
    {
        if (*buffer != NULL)  // 收到数据但喂狗失败
        {
            netbuf_delete(*buffer);  // 删掉收到的netbuf
            *buffer = NULL;  // 清指针
        }
        g_connected = false;  // 置断开
        return ERR_ABRT;  // 返回中止
    }
    return err == ERR_OK && *buffer == NULL ? ERR_CLSD : err;  // 收到NULL是对方关连接，否则返回原错误
}

/* ===== 下行命令处理 =====
 * 收到 PUBLISH 到 commands 主题时调。
 * 设计 5.1: 拒绝 RETAIN=1 的保留控制回放
 */
static void mqtt_handle_command(const uint8_t *body, uint32_t body_len, uint8_t publish_flags)  // 处理下行命令：解析JSON并执行
{
    /* 可变头：主题长度(2) + 主题；载荷：主题之后 */
    if (body_len < 4) return;  // 至少2字节主题长度+2字节数据，太短返回
    uint16_t tlen = ((uint16_t)body[0] << 8) | body[1];  // 大端拼16位主题长度
    if ((uint32_t)(2 + tlen) > body_len) return;  // 长度字段比实际载荷还大，坏数据返回

    const char *topic = (const char *)&body[2];  // 主题字符串起始位置
    const uint8_t *payload = body + 2 + tlen;  // JSON载荷起始位置
    uint32_t plen = body_len - 2 - tlen;  // 载荷长度=总长-2字节长度字段-主题长

    /* RETAIN 检查：设计 5.1 拒绝 RETAIN=1 的保留控制回放 */
    bool is_retain = (publish_flags & 0x01) != 0;  // 取固定头bit0的RETAIN标志
    if (is_retain)  // 是broker存的旧命令回放
    {
        printf("[MQTT] reject retained command (RETAIN=1)\r\n");  // 打印拒绝日志
        return;  // 旧命令不执行，直接返回
    }

    /* 只处理 commands 主题 */
    if (tlen != strlen(g_topic_commands) || memcmp(topic, g_topic_commands, tlen) != 0)  // 长度或内容不匹配
    {
        return;  // 不是commands主题，不处理
    }

    /* 拷贝载荷到 \0 结尾缓冲 */
    if (plen > MQTT_MAX_PAYLOAD || memchr(payload, '\0', plen) != NULL)  // 载荷超长或含\0
    {
        printf("[MQTT] invalid command payload: %lu\r\n", (unsigned long)plen);  // 打印非法载荷
        return;  // 非法数据返回
    }
    memcpy(rx_payload_buf, payload, plen);  // 拷贝JSON到全局命令缓冲
    rx_payload_buf[plen] = '\0';  // 强制字符串结尾，方便strstr等函数

    printf("[MQTT] recv cmd bytes=%lu\r\n", (unsigned long)plen);  // 打印收到命令字节数

#if GATEWAY_ENABLE_M2  // M2模块启用才编译分流
    /* ★OTA 升级通知分流：JSON 含 "url" + "sha256" → 升级通知，转给 NetworkUpgrade。
     * 升级通知和普通 Modbus 命令复用 commands topic，靠 JSON 字段区分：
     *   升级通知: {"task_id":"...","url":"http://...","sha256":"...","size":262144,...}
     *   普通命令: {"slave":2,"func":6,"addr":0,"val":100}
     * 不在回调里执行下载/Flash/密码运算（README 要求），只唤醒执行任务。 */
    if (strstr(rx_payload_buf, "\"url\"") && strstr(rx_payload_buf, "\"sha256\""))  // 同时有url和sha256就是OTA通知
    {
        int nr = NetworkUpgrade_Notify(rx_payload_buf);  // 通知升级模块，只唤醒任务不立刻下载
        char ack[96];  // 回执临时数组
        int alen;  // 回执长度
        if (nr == 0)  // 返回0受理成功
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"accepted\",\"type\":\"ota\"}");  // 拼受理成功回执
        }
        else if (nr == -2)  // 返回-2升级模块忙
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"busy\",\"type\":\"ota\"}");  // 拼忙回执
        }
        else  // 其他错误码
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"type\":\"ota\",\"err\":%d}", nr);  // 拼错误回执
        }
        MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)ack, (size_t)alen);  // 发回执到cmd_ack主题
        return;  // OTA处理完返回
    }

    /* ★配置变更分流：JSON 含 "slaves" → 新增/删除/修改从站，转给 Config 模块。
     * 配置变更和普通命令复用 commands topic，靠 JSON 字段区分：
     *   配置变更: {"op":"upsert","slaves":[{"id":5,"fc":3,"reg":0,...,"points":[...]}]}
     *   删除从站: {"op":"delete","slaves":[{"id":4}]}
     * Config 模块解析 JSON → mb_apply_slave_config 注册/删除从站 → 回 cmd_ack 回执 */
    if (strstr(rx_payload_buf, "\"slaves\""))  // 含slaves字段就是配置变更命令
    {
        int cr = Config_HandleMessage(rx_payload_buf);  // 交给Config模块处理
        char ack[128];  // 回执临时数组
        int alen;  // 回执长度
        if (cr == 0)  // 返回0受理成功
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"accepted\",\"type\":\"config\"}");  // 拼受理成功回执
        }
        else if (cr == -2)  // 返回-2模块忙
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"busy\",\"type\":\"config\",\"err\":\"upgrade\"}");  // 拼忙回执
        }
        else  // 其他错误码
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"type\":\"config\",\"err\":%d}", cr);  // 拼错误回执
        }
        MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)ack, (size_t)alen);  // 发回执到cmd_ack主题
        return;  // 配置处理完返回
    }
#endif  // 条件编译结束

    if (g_pause_requested || !UpgradeQuiesce_AllowsMutation())  // 暂停请求或升级静默期
    {
        static const char busy[] = "{\"status\":\"busy\",\"err\":\"upgrade\"}";  // 固定忙回执字符串
        MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)busy, sizeof(busy) - 1U);  // 发忙回执，减1去掉\0
        return;  // 不执行命令返回
    }

    /* 手写 JSON 解析：找 "slave":<int> "func":<int> "addr":<int> "val":<int> */
    int slave = -1, func = -1, addr = -1, val = 0;  // 四个字段默认值：从站-1、功能-1、地址-1、值0
    const char *keys[] = { "\"slave\"", "\"func\"", "\"addr\"", "\"val\"" };  // 要查找的4个JSON键
    int *outs[] = { &slave, &func, &addr, &val };  // 4个键对应的输出变量指针
    for (int k = 0; k < 4; k++)  // 循环解析4个字段
    {
        const char *p = strstr(rx_payload_buf, keys[k]);  // 在JSON里找键名
        if (!p) continue;  // 没找到跳过这个键
        p += strlen(keys[k]);  // 指针跳到键名之后
        while (*p == ' ' || *p == ':' || *p == '\t') p++;  // 跳过空格冒号制表符
        if (*p == '-' || (*p >= '0' && *p <= '9'))  // 是负号或数字开头
        {
            *outs[k] = atoi(p);  // 字符串转整数存入对应变量
        }
    }

    char ack[96];  // 回执临时数组
    int alen;  // 回执长度

    if (slave < 1 || slave > 247 || func < 0 || addr < 0)  // 从站范围1-247，功能地址不能负
    {
        alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":\"bad_cmd\"}");  // 拼坏命令回执
        MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)ack, (size_t)alen);  // 发坏命令回执
        return;  // 返回
    }

    uint16_t write_val = (uint16_t)val;  // 值转16位，Modbus寄存器是16位
    if (!mqtt_health_wait(3000U)) return;  // 登记3秒写等待
    mb_status_e st = mb_inject_write((uint8_t)slave,  // 注入写命令到Modbus队列，参数从站号
                                     (uint8_t)func,  // 功能码
                                     (uint16_t)addr,  // 寄存器地址
                                     1,  // 写1个寄存器
                                     &write_val,  // 要写的值指针
                                     g_write_sem,  // 完成后释放的二值信号量
                                     &g_write_err,  // 执行结果错误码输出
                                     3000);  // 超时3秒
    if (!mqtt_health_progress()) return;  // 注入后喂狗

    if (st == MB_STATUS_OK)  // 注入队列成功
    {
        if (!mqtt_health_wait(4000U)) return;  // 登记4秒等结果
        osStatus_t status = osSemaphoreAcquire(g_write_sem, pdMS_TO_TICKS(4000U));  // 等信号量，最多4秒
        if (!mqtt_health_progress()) return;  // 喂狗
        if (status == osOK)  // 信号量拿到，Modbus执行完成
        {
            int errcode = (int)g_write_err;  // 取执行结果错误码
            if (errcode == (int)ERR_OK_QUERY)  // 错误码是查询OK
            {
                alen = snprintf(ack, sizeof(ack), "{\"status\":\"ok\",\"err\":0}");  // 拼成功回执
            }
            else  // 执行有错误
            {
                alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":%d}", errcode);  // 拼错误回执
            }
        }
        else  // 信号量超时
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":\"timeout\"}");  // 拼超时回执
        }
    }
    else if (st == MB_QUEUE_FULL_ERR)  // Modbus队列满
    {
        alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":\"qfull\"}");  // 拼队列满回执
    }
    else  // 其他注入错误
    {
        alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":%d}", (int)st);  // 拼错误回执
    }

    MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)ack, (size_t)alen);  // 发最终回执到cmd_ack主题
}

// 使用已置备身份；尚未置备时沿用本次运行配置中的ClientID，不使用固定测试名。
bool MqttClient_GetGatewayId(char *out, size_t capacity)  // 获取网关ID：优先已置备身份，否则用配置里的客户端ID
{
    if (out == NULL || capacity == 0U) return false;  // 输出指针空或容量0，返回假
    if (storage_identity_get(out, capacity) != HAL_OK)  // 读已置备身份失败
    {
        const app_config_t *config = AppConfig_Get();  // 拿全局配置
        if (config == NULL || strlen(config->mq_client_id) >= capacity) return false;  // 配置空或ID放不下
        strcpy(out, config->mq_client_id);  // 用配置里的客户端ID当网关ID
    }
    if (out[0] == '\0') return false;  // 还是空，返回假
    for (const char *p = out; *p; p++)  // 逐个字符检查ID合法性
        if ((unsigned char)*p < 0x20U || *p == '"' || *p == '\\') return false;  // 含控制字符或引号反斜杠就非法
    return true;  // ID合法返回真
}

// newlib-nano不支持%llu；完整64位序号用十进制字符串编码。
static void mqtt_sequence_text(uint64_t value, char out[21])  // 64位序号转十进制字符串，写入21字节数组
{
    unsigned int end = 20U;  // 从数组末尾往前写
    out[end] = '\0';  // 先写字符串结尾符
    do  // do-while至少处理一位
    {
        out[--end] = (char)('0' + value % 10U);  // 取个位数字转字符，从后往前放
        value /= 10U;  // 去掉个位
    } while (value != 0U);  // 还有数就继续
    memmove(out, out + end, 21U - end);  // 把后面的数字移到数组开头
}

// 读取回执的无转义标识符；控制字符、反斜线和不完整字符串均拒绝。
static bool mqtt_ack_string(const char **cursor, const char **text, size_t *length)  // 解析JSON字符串，读一个引号内的值
{
    const char *p = *cursor + strspn(*cursor, " \t\r\n");  // 跳过空白
    if (*p++ != '"') return false;  // 不是引号开头，返回假
    *text = p;  // 字符串内容起始位置
    while (*p && *p != '"')  // 遍历到结尾或闭引号
    {
        if ((unsigned char)*p < 0x20U || *p == '\\') return false;  // 含控制字符或反斜杠就非法
        p++;  // 继续下一个字符
    }
    if (*p != '"') return false;  // 没找到闭引号，不完整字符串
    *length = (size_t)(p - *text);  // 计算字符串长度
    *cursor = p + 1;  // 光标移到闭引号之后
    return true;  // 解析成功返回真
}

// 整条回执通过后才交付位图；未知本机ID忽略，不接受其他网关或数字溢出。
static bool mqtt_ack_parse(const char *json, uint8_t *ack_mask)  // 解析业务回执JSON，输出确认位图
{
    char gateway[GATEWAY_ID_MAX_LEN];  // 本机网关ID临时数组
    if (!MqttClient_GetGatewayId(gateway, sizeof(gateway))) return false;  // 拿本机网关ID失败返回假
    size_t gateway_len = strlen(gateway);  // 网关ID长度
    const char *p = json + strspn(json, " \t\r\n");  // 跳过开头空白
    if (*p++ != '{') return false;  // 不是花括号开头返回假
    unsigned int fields = 0U;  // 已解析字段位图
    uint8_t matched = 0U;  // 本批已确认记录的位图
    for (;;)  // 循环解析键值对
    {
        const char *key;  // 键字符串
        size_t key_len;  // 键长度
        if (!mqtt_ack_string(&p, &key, &key_len)) return false;  // 解析键名失败返回假
        p += strspn(p, " \t\r\n");  // 跳过空白
        if (*p++ != ':') return false;  // 不是冒号返回假
        p += strspn(p, " \t\r\n");  // 跳过空白
        unsigned int field;  // 当前字段编号
        if (key_len == 14U && memcmp(key, "schema_version", key_len) == 0)  // 键是schema_version
        {
            field = 1U;  // 字段编号1
            if (*p++ != '1') return false;  // 值必须是字符'1'，否则返回假
        }
        else if (key_len == 10U && memcmp(key, "gateway_id", key_len) == 0)  // 键是gateway_id
        {
            field = 2U;  // 字段编号2
            const char *id;  // 网关ID字符串
            size_t len;  // 长度
            if (!mqtt_ack_string(&p, &id, &len) || len != gateway_len ||  // 解析失败或长度不等
                memcmp(id, gateway, len) != 0) return false;  // 内容不是本机ID返回假
        }
        else if (key_len == 6U && memcmp(key, "ack_id", key_len) == 0)  // 键是ack_id
        {
            field = 4U;  // 字段编号4
            const char *id;  // ack_id字符串
            size_t len;  // 长度
            if (!mqtt_ack_string(&p, &id, &len) || len == 0U) return false;  // 解析失败或空返回假
        }
        else if (key_len == 10U && memcmp(key, "record_ids", key_len) == 0)  // 键是record_ids
        {
            field = 8U;  // 字段编号8
            if (*p++ != '[') return false;  // 必须是数组开头方括号
            p += strspn(p, " \t\r\n");  // 跳过空白
            for (unsigned int count = 0U; *p != ']'; count++)  // 循环解析数组元素
            {
                const char *id;  // 记录ID字符串
                size_t len;  // 长度
                if (count >= 32U || !mqtt_ack_string(&p, &id, &len) || len <= gateway_len + 5U ||  // 最多32个或格式非法
                    memcmp(id, gateway, gateway_len) != 0 ||  // 前缀不是本机网关ID
                    memcmp(id + gateway_len, ":seq:", 5U) != 0) return false;  // 中间不是:seq:
                const char *digit = id + gateway_len + 5U;  // 序号数字起始
                const char *end = id + len;  // 记录ID结束
                if (*digit == '0' && digit + 1 != end) return false;  // 前导0非法
                uint64_t seq = 0U;  // 解析出的序号
                while (digit != end)  // 逐字符转数字
                {
                    if (*digit < '0' || *digit > '9') return false;  // 非数字返回假
                    unsigned int value = (unsigned int)(*digit++ - '0');  // 字符转数字
                    if (seq > (UINT64_MAX - value) / 10U) return false;  // 溢出保护
                    seq = seq * 10U + value;  // 累加序号
                }
                for (unsigned int i = 0U; i < g_fetch_resp.count; i++)  // 遍历本批已发记录
                    if ((g_replay_sent_mask & (1U << i)) && g_fetch_resp.records[i].seq == seq)  // 是本批已发且序号匹配
                        matched |= (uint8_t)(1U << i);  // 标记这条已被确认
                p += strspn(p, " \t\r\n");  // 跳过空白
                if (*p == ']') break;  // 数组结束跳出
                if (*p++ != ',') return false;  // 不是逗号返回假
                p += strspn(p, " \t\r\n");  // 跳过空白
                if (*p == ']') return false;  // 逗号后直接结束非法
            }
            p++;  // 跳过闭方括号
        }
        else return false;  // 未知字段返回假
        if (fields & field) return false;  // 字段重复返回假
        fields |= field;  // 记录已解析字段
        p += strspn(p, " \t\r\n");  // 跳过空白
        if (*p == '}') break;  // 对象结束跳出
        if (*p++ != ',') return false;  // 不是逗号返回假
    }
    p++;  // 跳过闭花括号
    p += strspn(p, " \t\r\n");  // 跳过空白
    if (*p != '\0' || fields != 15U) return false;  // 还有残留或字段不全返回假
    *ack_mask = matched;  // 输出确认位图
    return true;  // 解析成功返回真
}

// 只登记本批已发送记录的明确业务确认；PUBACK不触及存储水位。
static void mqtt_handle_ack(const uint8_t *body, uint32_t body_len)  // 处理业务回执PUBLISH，解析确认位图
{
    if (body_len < 4U) return;  // 至少2字节长度+2字节内容
    uint16_t tlen = ((uint16_t)body[0] << 8U) | body[1];  // 拼主题长度
    if ((uint32_t)tlen + 2U > body_len || tlen != strlen(g_topic_acks) ||  // 长度超界或长度不匹配
        memcmp(body + 2, g_topic_acks, tlen) != 0) return;  // 不是acks主题返回
    uint32_t plen = body_len - 2U - tlen;  // JSON载荷长度
    if (plen > MQTT_MAX_PAYLOAD || memchr(body + 2U + tlen, '\0', plen) != NULL) return;  // 载荷超长或含\0
    memcpy(rx_payload_buf, body + 2U + tlen, plen);  // 拷贝JSON到命令缓冲
    rx_payload_buf[plen] = '\0';  // 强制结尾符
    uint8_t ack_mask;  // 确认位图
    if (!mqtt_ack_parse(rx_payload_buf, &ack_mask))  // 解析JSON失败
    {
        printf("[MQTT] invalid business ack\r\n");  // 打印非法回执
        return;  // 返回
    }
    if (!g_pause_requested && UpgradeQuiesce_AllowsMutation())  // 未暂停且允许修改
        g_replay_ack_mask |= ack_mask;  // 合并确认位图，标记这些记录已被确认
}

/* 收到 PUBACK：匹配 QoS1 pending 队列，释放 packet_id
 * 设计 5.2: PUBACK 只释放 MQTT packet_id，不回收业务缓存（业务回执才回收） */
static void mqtt_handle_puback(uint16_t packet_id)  // 处理QoS1的PUBACK，释放报文ID
{
    if (!mqtt_lock_tx())  // 拿发送锁
        return;  // 拿锁失败返回
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++)  // 遍历QoS1待确认队列
    {
        if (s_qos1_pending[i].in_use && s_qos1_pending[i].packet_id == packet_id)  // 匹配到使用中的报文ID
        {
            if (packet_id == g_boot_result_packet_id)  // 是开机结果那个报文ID
            {
                g_boot_result_acked = true;  // 标记开机结果已确认
                g_boot_result_packet_id = 0;  // 清报文ID
            }
            s_qos1_pending[i].in_use = false;  // 释放这个队列槽位
            printf("[MQTT] PUBACK pid=%u\r\n", (unsigned)packet_id);  // 打印收到PUBACK
            break;  // 找到就跳出
        }
    }
    osMutexRelease(g_tx_mutex);  // 释放发送锁
}

/* ===== 离线补传驱动 =====
 * 设计 5.2:
 *   - 在线立即请求存储任务读取已提交记录，按现有 Topic 和 JSON 契约发布
 *   - PUBACK 后仅释放 MQTT packet_id，不回收缓存（业务回执才回收）
 *   - 回执 30s 未到则保持原 ID 重发，重试间隔不超过 60s
 *   - 恢复后每次取最多 STORAGE_M2_FETCH_BATCH_MAX 条
 */
static bool g_fetch_in_flight = false;  // 是否有取记录请求在途
static uint32_t g_fetch_req_id = 1;  // 取记录请求ID，每次加1
static uint32_t g_fetch_expected_id;  // 期望的应答ID，用来过滤过期应答


/* 只接纳匹配的旧请求应答；读取副本不回收存储中的未确认记录。 */
static bool mqtt_take_fetch_response(void)  // 从队列取一批补传记录，校验请求ID
{
    if (osMessageQueueGet(storage_m2_fetch_resp_queue(), &g_fetch_resp, NULL, 0) != osOK)  // 非阻塞取队列
        return false;  // 队列空返回假
    if (!g_fetch_in_flight || g_fetch_resp.req_id != g_fetch_expected_id)  // 没有在途请求或ID不匹配
    {
        printf("[MQTT] stale fetch response id=%lu expected=%lu\r\n",  // 打印过期应答
               (unsigned long)g_fetch_resp.req_id, (unsigned long)g_fetch_expected_id);
        g_fetch_resp.count = 0U;  // 清掉这条过期应答
        return false;  // 返回假
    }
    g_fetch_in_flight = false;  // 在途请求完成
    if (g_fetch_resp.count > STORAGE_M2_FETCH_BATCH_MAX)  // 条数超上限
    {
        g_fetch_resp.count = 0U;  // 清空
        return false;  // 返回假
    }
    g_replay_sent_mask = 0U;  // 清已发送位图
    g_replay_ack_mask = 0U;  // 清已确认位图
    g_replay_last_seq = 0U;  // 清最后确认序号
    g_replay_last_tick = 0U;  // 清最后发送时间
    return true;  // 取批成功返回真
}

/* 本任务在业务处理之间确认暂停，退避期间也接收已发出的补传应答。 */
static void mqtt_pause_checkpoint(void)  // 暂停检查点：确认是否已真正暂停
{
    if (g_pause_requested)  // 有暂停请求
        (void)mqtt_take_fetch_response();  // 收一下在途应答，避免卡住
    taskENTER_CRITICAL();  // 关中断，保护共享标志
    g_pause_acknowledged = g_pause_requested && !g_fetch_in_flight && mqtt_health_progress();  // 暂停请求+无在途+健康才算确认暂停
    taskEXIT_CRITICAL();  // 开中断
}

/* 保活、命令拒绝回执及升级状态可继续发送；业务发送复查最终准入。 */
static bool mqtt_allows_publish(const char *topic)  // 判断当前能否发这个主题
{
    return strcmp(topic, g_topic_status) == 0 || strcmp(topic, g_topic_cmd_ack) == 0 ||  // 状态主题可发
           strcmp(topic, g_topic_ota_status) == 0 ||  // 升级状态可发
           (!g_pause_requested && UpgradeQuiesce_AllowsMutation());  // 其他主题需未暂停且允许修改
}

/* 设置请求时清除旧确认，防止取消后再次暂停复用旧状态。 */
void MqttClient_SetPaused(bool paused)  // 外部设置暂停状态
{
    taskENTER_CRITICAL();  // 关中断
    g_pause_requested = paused;  // 更新暂停请求
    g_pause_acknowledged = false;  // 清确认标志
    taskEXIT_CRITICAL();  // 开中断
}

/* 只观察任务确认及既有发送锁，不代报MQTT任务健康。 */
bool MqttClient_IsPaused(void)  // 查询是否已真正暂停
{
    taskENTER_CRITICAL();  // 关中断
    bool paused = g_pause_requested && g_pause_acknowledged;  // 请求暂停且任务已确认
    taskEXIT_CRITICAL();  // 开中断
    if (!paused || g_tx_mutex == NULL || osMutexAcquire(g_tx_mutex, 0U) != osOK)  // 没暂停或锁不可用
        return false;  // 返回假
    osMutexRelease(g_tx_mutex);  // 能拿到锁说明空闲，释放
    return true;  // 返回真
}

// 仅回收明确确认的批次前缀；乱序确认留在原批次，不能跨过未确认项。
static void mqtt_replay_pump(void)  // 离线补传主驱动：确认前缀、重发、取下一批
{
    if (g_fetch_in_flight) (void)mqtt_take_fetch_response();  // 有在途请求先收应答
    if (g_pause_requested || !UpgradeQuiesce_AllowsMutation()) return;  // 暂停或静默期不补传

    unsigned int confirmed = 0U;  // 连续确认的条数
    while (confirmed < g_fetch_resp.count && (g_replay_ack_mask & (1U << confirmed))) confirmed++;  // 从前往后数连续确认
    if (confirmed != 0U)  // 有连续确认的前缀
    {
        storage_ack_t ack = {.ack_upto_seq = g_fetch_resp.records[confirmed - 1U].seq};  // 确认到前缀最后一条的序号
        if (ack.ack_upto_seq > g_replay_last_seq)  // 序号比上次大才推进
        {
            taskENTER_CRITICAL();  // 关中断
            bool allowed = !g_pause_requested && UpgradeQuiesce_AllowsMutation();  // 再次确认允许
            osStatus_t st = allowed ? osMessageQueuePut(storage_m2_ack_queue(), &ack, 0, 0) : osErrorResource;  // 发确认到存储队列
            taskEXIT_CRITICAL();  // 开中断
            if (st != osOK) return;  // 发失败返回
            g_replay_last_seq = ack.ack_upto_seq;  // 更新最后确认序号
        }
        if (confirmed == g_fetch_resp.count) g_fetch_resp.count = 0U;  // 整批都确认了，清空
    }

    uint32_t now = osKernelGetTickCount();  // 当前tick
    bool resend = (uint32_t)(now - g_replay_last_tick) >= MQTT_REPLAY_RESEND_MS;  // 距上次发送超30秒则重发
    uint8_t before = g_replay_sent_mask;  // 记发送前位图
    bool sent = false;  // 本次是否发了
    for (unsigned int i = 0U; i < g_fetch_resp.count; i++)  // 遍历本批记录
    {
        uint8_t bit = (uint8_t)(1U << i);  // 第i条对应位
        if ((g_replay_ack_mask & bit) || ((g_replay_sent_mask & bit) && !resend)) continue;  // 已确认或未到重发期跳过
        record_out_t *r = &g_fetch_resp.records[i];  // 取第i条记录
        const char *topic = r->kind == RECORD_KIND_EVENT ? g_topic_events : g_topic_telemetry;  // 事件类发events，遥测发telemetry
        if (mqtt_publish_qos1(topic, r->payload, r->payload_len, false, r->seq) != 0) break;  // 发QoS1，失败停
        g_replay_sent_mask |= bit;  // 标记这条已发送
        sent = true;  // 置已发送标志
    }
    if (sent && (before == 0U || resend)) g_replay_last_tick = now;  // 发过且是首次或重发，更新发送时间

    // 同一批未确认前不覆盖原应答，保留序号、数据和乱序确认依据。
    if (g_fetch_resp.count == 0U && !g_fetch_in_flight)  // 本批处理完且无在途
    {
        storage_fetch_req_t req = {.req_id = g_fetch_req_id, .max_count = STORAGE_M2_FETCH_BATCH_MAX};  // 构造取下一批请求
        taskENTER_CRITICAL();  // 关中断
        bool allowed = !g_pause_requested && UpgradeQuiesce_AllowsMutation();  // 确认允许
        if (allowed && osMessageQueuePut(storage_m2_fetch_req_queue(), &req, 0, 0) == osOK)  // 发取批请求
        {
            g_fetch_in_flight = true;  // 置在途
            g_fetch_expected_id = req.req_id;  // 记期望ID
            if (++g_fetch_req_id == 0U) g_fetch_req_id = 1U;  // 请求ID加1，溢出回1
        }
        taskEXIT_CRITICAL();  // 开中断
    }
}

/* QoS1 超时重发检查（设计 5.2: 30s 未收到 PUBACK 则保持原 packet_id 重发，DUP=1）
 * 在主循环 ERR_TIMEOUT 分支和补传 pump 一起调用 */
static void mqtt_qos1_check_resend(void)  // QoS1超时重发检查
{
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++)  // 遍历QoS1待确认队列
    {
        if (!mqtt_lock_tx())  // 拿发送锁
            return;  // 拿锁失败返回
        uint32_t now = osKernelGetTickCount();  // 当前tick
        if (!s_qos1_pending[i].in_use || !mqtt_allows_publish(s_qos1_pending[i].topic) ||  // 没在用或不允许发
            (now - s_qos1_pending[i].send_tick) < MQTT_REPLAY_RESEND_MS)  // 还没到30秒
        {
            osMutexRelease(g_tx_mutex);  // 释放锁
            continue;  // 跳过这一条
        }

        /* 30s 超时，重发（保持原 packet_id，DUP=1） */
        size_t pkt_len = mqtt_build_publish(s_qos1_pending[i].topic,  // 用原主题
                                            s_qos1_pending[i].payload,  // 原载荷
                                            s_qos1_pending[i].payload_len,  // 原长度
                                            s_qos1_pending[i].retain, 1,  // 原retain，QoS1
                                            s_qos1_pending[i].packet_id, true);  // 原报文ID，DUP置1
        err_t err = mqtt_write(tx_buf, pkt_len);  // 发送
        if (err == ERR_OK)  // 发送成功
        {
            s_qos1_pending[i].send_tick = now;  // 更新发送时间
            printf("[MQTT] QoS1 resend pid=%u (30s timeout)\r\n",  // 打印重发
                   (unsigned)s_qos1_pending[i].packet_id);
        }
        osMutexRelease(g_tx_mutex);  // 释放锁
        if (err != ERR_OK) return;  // 发送失败返回
    }
}

/* ===== 对外接口实现 ===== */

AppModuleInitResult MqttClient_Init(void)  // 初始化MQTT模块：创建锁和信号量、加载配置拼主题
{
    if (g_tx_mutex != NULL && g_write_sem != NULL)  // 两个同步对象都已创建
        return APP_MODULE_INIT_OK;  // 已经初始化过，直接返回成功

    if (g_tx_mutex == NULL)  // 发送锁还没创建
    {
        g_tx_mutex = osMutexNew(&g_tx_mutex_attr);  // 创建互斥锁
        if (g_tx_mutex == NULL)  // 创建失败
            return APP_MODULE_INIT_FAILED;  // 返回失败
    }

    g_write_sem = osSemaphoreNew(1, 0, NULL);  // 创建写完成信号量，最大1初值0
    if (g_write_sem == NULL)  // 创建失败
    {
        if (osMutexDelete(g_tx_mutex) == osOK)  // 删除之前创建的锁
            g_tx_mutex = NULL;  // 清锁指针
        return APP_MODULE_INIT_FAILED;  // 返回失败
    }

    srand(osKernelGetTickCount());   // 用当前tick做随机种子，退避抖动用

    mqtt_config_load();  // 加载MQTT运行时配置

#if MQTT_USE_BAIDU  // 百度云模式
    /* ★★2026-11-10 关键修复：必须先调 baidu_build_auth() 设置 g_cfg.client_id=DeviceKey，
     * 再调 mqtt_topics_build() 拼 topic。之前顺序反了：topics_build 用空 client_id
     * → Will Topic 变成 "$iot//user/status"（DeviceKey 丢失）→ CONNACK rc=2。
     * baidu_build_auth 里的 timestamp 每次连接前会重算（在 task 循环里），这里只是
     * 初始化阶段把 client_id/username/password 填上，让 topics_build 能拿到正确的 DeviceKey。 */
    baidu_build_auth();  // 先认证，把client_id填成DeviceKey
#endif  // 条件编译结束

    mqtt_topics_build();  // 再拼7个主题

    g_conn = NULL;  // 连接句柄空
    g_connected = false;  // 未连接
    rx_ring_len = 0;  // 接收环缓冲清空
    g_fetch_in_flight = false;  // 无在途取批请求
    g_backoff_attempt = 0;  // 退避次数归零
    g_replay_last_seq = 0;  // 补传最后序号归零
    g_replay_last_tick = 0;  // 补传最后发送时间归零
    g_fetch_resp.count = 0U;  // 本批记录数归零
    g_replay_sent_mask = 0U;  // 已发送位图归零
    g_replay_ack_mask = 0U;  // 已确认位图归零
    return APP_MODULE_INIT_OK;  // 初始化成功
}

bool MqttClient_IsConnected(void)  // 查询当前是否已连接
{
    return g_connected;  // 返回连接标志
}

int MqttClient_Publish(const char *topic, const uint8_t *payload, size_t plen)  // 对外QoS0发布
{
    if (g_tx_mutex == NULL)  // 发送锁还没创建
    {
        return -1;  // 返回失败
    }

    /* 超过4 KiB的载荷明确拒绝，不截断处理。 */
    if (plen > MQTT_MAX_PAYLOAD)  // 载荷超4KB
    {
        return -3;  // 返回载荷超限
    }
    size_t tlen = strlen(topic);  // 算主题长度
    if (tlen + plen + 8 > sizeof(tx_buf))  // 主题+载荷+头部超发送缓冲
    {
        return -3;   // 报文装不下，返回超限
    }

    if (!mqtt_lock_tx())  // 拿发送锁
        return -1;  // 拿锁失败返回
    if (!g_connected || g_conn == NULL)  // 未连接或句柄空
    {
        osMutexRelease(g_tx_mutex);  // 释放锁
        return -1;  // 返回失败
    }

    if (!mqtt_allows_publish(topic))  // 当前不允许发这个主题
    {
        osMutexRelease(g_tx_mutex);  // 释放锁
        return -5;  // 返回不允许
    }

    size_t pkt_len = mqtt_build_publish(topic, payload, plen, false, 0, 0, false);   // 组QoS0报文，无retain无DUP
    err_t err = mqtt_write(tx_buf, pkt_len);  // 发送

    osMutexRelease(g_tx_mutex);  // 释放锁
    return (err == ERR_OK) ? 0 : -2;  // 发送成功返回0，失败返回-2
}

/* 共用原QoS1发送路径；启动结果等匹配PUBACK后才返回已确认。 */
static int mqtt_publish_qos1(const char *topic, const uint8_t *payload, size_t plen,
                             bool boot_result, uint64_t record_seq)  // QoS1发布，boot_result表示启动结果，record_seq是补传序号
{
    if (g_tx_mutex == NULL)  // 发送锁还没创建
    {
        return -1;  // 返回失败
    }

    // 正文上限与存储一致；pending另预留发送时补充的记录元数据。
    if (plen > RECORD_PAYLOAD_MAX || (record_seq != 0U &&  // 正文超限，或补传时
        (plen < 2U || payload[0] != '{' || payload[plen - 1U] != '}')))  // 不是完整JSON花括号
    {
        return -3;   // 返回载荷超限
    }
    size_t tlen = strlen(topic);  // 算主题长度
    if (tlen + plen + 10 > sizeof(tx_buf))  // 主题+载荷+头部超发送缓冲
    {
        return -3;   // 报文装不下，返回超限
    }
    if (plen > MQTT_QOS1_PAYLOAD_MAX)  // 载荷超QoS1 pending上限
    {
        return -3;   // 返回超限
    }

    if (!mqtt_lock_tx())  // 拿发送锁
        return -1;  // 拿锁失败返回
    if (boot_result && (g_boot_result_acked || (g_boot_result_packet_id != 0U)))  // 启动结果已确认或还在途
    {
        int result = g_boot_result_acked ? 1 : 0;  // 已确认返回1，在途返回0
        osMutexRelease(g_tx_mutex);  // 释放锁
        return result;  // 返回状态
    }
    if (!g_connected || g_conn == NULL)  // 未连接
    {
        osMutexRelease(g_tx_mutex);  // 释放锁
        return -1;  // 返回失败
    }

    /* 找空闲 pending 槽 */
    if (!mqtt_allows_publish(topic))  // 不允许发这个主题
    {
        osMutexRelease(g_tx_mutex);  // 释放锁
        return -5;  // 返回不允许
    }
    int slot = -1;  // 空闲槽下标
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++)  // 遍历pending队列
    {
        if (!s_qos1_pending[i].in_use) { slot = i; break; }  // 找到第一个空闲槽
    }
    if (slot < 0)  // 没找到空闲槽
    {
        osMutexRelease(g_tx_mutex);  // 释放锁
        return -4;   // pending队列满
    }

    if (record_seq != 0U)  // 是补传记录，需要加元数据头
    {
        char gateway[GATEWAY_ID_MAX_LEN], sequence[21];  // 网关ID和序号字符串
        if (!MqttClient_GetGatewayId(gateway, sizeof(gateway)))  // 拿网关ID失败
        {
            osMutexRelease(g_tx_mutex);  // 释放锁
            return -3;  // 返回超限
        }
        mqtt_sequence_text(record_seq, sequence);  // 序号转字符串
        int prefix = snprintf((char *)s_qos1_pending[slot].payload, MQTT_RECORD_HEADER_MAX,  // 拼元数据头
            "{\"gateway_id\":\"%s\",\"record_id\":\"%s:seq:%s\",\"seq\":\"%s\",",  // gateway_id、record_id、seq三个字段
            gateway, gateway, sequence, sequence);
        if (prefix < 0 || prefix >= (int)MQTT_RECORD_HEADER_MAX ||  // 拼接失败或超限
            (size_t)prefix + plen - 1U > MQTT_QOS1_PAYLOAD_MAX)  // 加正文后超限
        {
            osMutexRelease(g_tx_mutex);  // 释放锁
            return -3;  // 返回超限
        }
        size_t member = 1U;  // 从第2个字符开始扫
        while (member + 1U < plen && (payload[member] == ' ' || payload[member] == '\t' ||  // 跳过空白
               payload[member] == '\r' || payload[member] == '\n')) member++;
        if (member == plen - 1U) prefix--; // 空对象不保留元数据后的分隔逗号。
        // 复用原pending槽加元数据；TF上的旧正文、序号和摘要不改写。
        memcpy(s_qos1_pending[slot].payload + prefix, payload + 1U, plen - 1U);  // 去掉原开头的{，拼在元数据后
        plen += (size_t)prefix - 1U;  // 更新总长度
    }
    else  // 普通发布
    {
        memcpy(s_qos1_pending[slot].payload, payload, plen);  // 直接拷贝载荷
    }
    payload = s_qos1_pending[slot].payload;  // payload指向pending槽

    /* 跳过仍在途的编号，避免回绕后将其他发布的PUBACK当成启动结果确认。 */
    uint16_t pid;  // 报文ID
    bool id_in_use;  // ID是否在用
    do  // do-while至少分配一次
    {
        pid = g_next_packet_id++;  // 取ID再自增
        if (g_next_packet_id == 0) g_next_packet_id = 1;  // 溢出回1
        id_in_use = false;  // 先假设没在用
        for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++)  // 遍历pending队列
        {
            if (s_qos1_pending[i].in_use && s_qos1_pending[i].packet_id == pid)  // 有槽位用了这个ID
                id_in_use = true;  // 标记在用
        }
    } while (id_in_use);  // ID被占用就重新分配

    /* 存 pending（拷贝 payload，函数返回后调用方数据可释放） */
    s_qos1_pending[slot].in_use      = true;  // 标记槽位使用中
    s_qos1_pending[slot].packet_id   = pid;  // 存报文ID
    s_qos1_pending[slot].send_tick   = osKernelGetTickCount();  // 存发送时间
    s_qos1_pending[slot].retain      = false;  // 不retain
    s_qos1_pending[slot].payload_len = plen;  // 存载荷长度
    strncpy(s_qos1_pending[slot].topic, topic, sizeof(s_qos1_pending[slot].topic) - 1);  // 存主题
    s_qos1_pending[slot].topic[sizeof(s_qos1_pending[slot].topic) - 1] = '\0';  // 强制结尾符
    /* 发送 QoS1 PUBLISH */
    size_t pkt_len = mqtt_build_publish(topic, payload, plen, false, 1, pid, false);  // 组QoS1报文
    err_t err = mqtt_write(tx_buf, pkt_len);  // 发送

    if (err != ERR_OK)  // 发送失败
    {
        s_qos1_pending[slot].in_use = false;  // 释放槽位
        osMutexRelease(g_tx_mutex);  // 释放锁
        return -2;  // 返回发送失败
    }
    if (boot_result)  // 是启动结果
        g_boot_result_packet_id = pid;  // 记启动结果报文ID，等PUBACK
    osMutexRelease(g_tx_mutex);  // 释放锁
    return 0;  // 返回成功
}

/* 普通QoS1发布保持原返回语义：0表示已发送，尚未确认。 */
int MqttClient_PublishQos1(const char *topic, const uint8_t *payload, size_t plen)  // 对外QoS1发布接口
{
    return mqtt_publish_qos1(topic, payload, plen, false, 0U);  // 非启动结果，无补传序号
}

/* 重复提交同一启动结果只查询在途状态，断线后由调用方以相同身份重发。 */
int MqttClient_PublishUpgradeResult(const uint8_t *payload, size_t plen)  // 发布升级结果，启动结果类型
{
    if ((payload == NULL) || (plen == 0U) || (g_topic_ota_status[0] == '\0'))  // 载荷空或主题未初始化
        return -3;  // 返回超限
    return mqtt_publish_qos1(g_topic_ota_status, payload, plen, true, 0U);  // 发到ota_status主题，启动结果
}

int MqttClient_ReportEvent(const char *event_type, const char *detail)  // 对外上报事件
{
    char json[128];  // 事件JSON临时数组
    int len = snprintf(json, sizeof(json), "{\"type\":\"%s\",\"detail\":\"%s\"}",  // 拼type和detail字段
                       event_type ? event_type : "",  // 事件类型，空则空串
                       detail ? detail : "");  // 详情，空则空串
    if (len <= 0) return -1;  // 拼接失败返回-1
    return MqttClient_Publish(g_topic_events, (const uint8_t *)json, (size_t)len);  // QoS0发到events主题
}

/* ===== 主任务 ===== */

static bool wait_network_ready(uint32_t timeout_ms)  // 等网络就绪：链路up且拿到有效IP，最多等timeout毫秒
{
    uint32_t started_ms = HAL_GetTick();  // 记录开始时间
    while ((uint32_t)(HAL_GetTick() - started_ms) < timeout_ms)  // 没超时就循环
    {
        uint32_t poll_ms = HAL_GetTick();  // 本次轮询开始时间
        if (!mqtt_health_wait(1000U))  // 登记1秒等待
            return false;  // 失败返回假
        int32_t link = m2_ethernet_link_wait(1000U);  // 等链路，最多1秒
        if (!mqtt_health_progress())  // 喂狗
            return false;  // 失败返回假
        if (link == ETH_OK)  // 链路已连上
        {
            if (netif_default != NULL &&  // 默认网口存在
                netif_is_up(netif_default) &&  // 网口已启用
                !ip4_addr_isany(netif_ip4_addr(netif_default)))  // IP不是0.0.0.0
            {
                return true;  // 网络就绪返回真
            }
        }
        uint32_t elapsed = (uint32_t)(HAL_GetTick() - poll_ms);  // 本次已耗时
        if (elapsed < 1000U && !mqtt_delay(1000U - elapsed))  // 补足1秒周期，延时喂狗
            return false;  // 延时失败返回假
    }
    return false;  // 超时返回假
}

/* 关闭发送准入后与发布者互斥删除；失败保留连接，禁止覆盖最后的资源引用。 */
static bool mqtt_connection_delete(void)  // 删除当前TCP连接，释放资源
{
    g_connected = false;  // 先置断开，阻止新发布
    if (!mqtt_lock_tx())  // 拿发送锁，与发布者互斥
        return false;  // 拿锁失败返回假

    if (!mqtt_health_wait(20000U))  // 登记20秒删除等待
    {
        osMutexRelease(g_tx_mutex);  // 释放锁
        return false;  // 返回假
    }
    /* 连接内的QoS1身份不可跨重连使用；业务层保留原始数据并重发。 */
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++)  // 遍历QoS1队列
        s_qos1_pending[i].in_use = false;  // 全部释放，跨连接不保留报文ID
    g_boot_result_packet_id = 0;  // 清启动结果报文ID
    err_t err = netconn_delete(g_conn);  // 删除连接
    if (err == ERR_OK)  // 删除成功
        g_conn = NULL;  // 句柄置空
    bool healthy = mqtt_health_progress();  // 喂狗
    osMutexRelease(g_tx_mutex);  // 释放锁
    if (err != ERR_OK)  // 删除失败
        printf("[MQTT] connection delete failed err=%d\r\n", (int)err);  // 打印失败，保留连接
    return err == ERR_OK && healthy;  // 删除成功且健康才返回真
}

/* MQTT任务独立登记监督，连接与业务状态机推进后才报告自身进展。 */
void MqttClient_Task(void *argument)  // MQTT主任务：连接、收发、心跳、补传的总循环
{
    (void)argument;  // 忽略参数
    g_mqtt_thread = osThreadGetId();  // 记本任务线程ID，供锁错误判断用
    if (!Monitor_RegisterHealth(APP_MODULE_MQTT, 2000U, 30000U))  // 注册健康监督，2秒周期30秒超时
        goto health_fail;  // 注册失败跳到健康失败
    bool config_loaded = false;  // 配置是否已加载
    printf("[MQTT] task started, broker=%s:%u cid=%s auth=%s\r\n",  // 打印启动信息
           g_cfg.broker_host, g_cfg.broker_port, g_cfg.client_id,  // broker地址端口客户端ID
           g_cfg.username[0] ? "yes" : "no");  // 是否认证

    while (1)  // 主循环
    {
        /* 无有效运行配置时只维持监督，不使用编译期默认平台启动业务。 */
        if (!App_IsReady(APP_RESOURCE_CONFIG))  // 配置还没就绪
        {
            if (!mqtt_delay(500U)) goto health_fail;  // 延时500毫秒喂狗
            continue;  // 继续等待
        }
        if (!config_loaded)  // 配置还没加载
        {
            mqtt_config_load();  // 加载配置
#if MQTT_USE_BAIDU  // 百度云
            baidu_build_auth();  // 认证
#endif  // 条件编译结束
            mqtt_topics_build();  // 拼主题
            config_loaded = true;  // 置已加载
        }
        if (g_conn != NULL && !mqtt_connection_delete())  // 有旧连接先删，删失败
        {
            if (!mqtt_delay(mqtt_backoff_delay(g_backoff_attempt++))) goto health_fail;  // 退避后重试
            continue;  // 继续循环
        }

        /* ---- 1. 等网络就绪 ---- */
        if (!wait_network_ready(60000))  // 等网络最多60秒
        {
            printf("[MQTT] network not ready, retry\r\n");  // 打印网络未就绪
            if (!mqtt_delay(5000)) goto health_fail;  // 延时5秒
            continue;  // 继续循环
        }
        printf("[MQTT] network ready, IP=%s\r\n",  // 打印网络就绪和IP
               ipaddr_ntoa(netif_ip4_addr(netif_default)));

        /* ---- 2. TCP 连接 broker ---- */
        /* DNS 解析 broker 域名 → IP（同步，走 tcpip 线程；LWIP_DNS=1） */
        printf("[MQTT] resolving %s ...\r\n", g_cfg.broker_host);  // 打印开始DNS解析
        /* 当前DNS两服务器各最多7秒重试，20秒覆盖应答/失败返回。 */
        if (!mqtt_health_wait(20000U)) goto health_fail;  // 登记20秒DNS等待
        err_t dnserr = netconn_gethostbyname(g_cfg.broker_host, &g_cfg.broker_ip);  // DNS解析域名到IP
        if (!mqtt_health_progress()) goto health_fail;  // 喂狗
        if (dnserr != ERR_OK)  // 解析失败
        {
            printf("[MQTT] DNS resolve '%s' failed err=%d\r\n", g_cfg.broker_host, (int)dnserr);  // 打印解析失败
            if (!mqtt_delay(mqtt_backoff_delay(g_backoff_attempt++))) goto health_fail;  // 退避后重试
            continue;  // 继续循环
        }
        g_cfg.broker_ip_valid = true;  // 置IP有效
        printf("[MQTT] DNS %s -> %s, connecting ...\r\n",  // 打印解析结果
               g_cfg.broker_host, ipaddr_ntoa(&g_cfg.broker_ip));

#if MQTT_USE_BAIDU  // 百度云
        /* 百度云：每次连接重算签名（timestamp 变化） */
        baidu_build_auth();  // 重算认证
#endif  // 条件编译结束

        if (!mqtt_health_wait(1000U)) goto health_fail;  // 登记1秒等待
        g_conn = netconn_new_with_callback(NETCONN_TCP, mqtt_netconn_event);  // 新建TCP连接并挂事件回调
        if (!mqtt_health_progress()) goto health_fail;  // 喂狗
        if (g_conn == NULL)  // 创建失败
        {
            printf("[MQTT] netconn_new failed\r\n");  // 打印失败
            if (!mqtt_delay(mqtt_backoff_delay(g_backoff_attempt++))) goto health_fail;  // 退避重试
            continue;  // 继续循环
        }

        /* ★关键：设 recv 超时 1 秒，主循环才能定期醒来发 PINGREQ 心跳 + 跑补传 pump。
           不设的话 netconn_recv 永久阻塞 → 心跳不发 → keepalive(30s) 超时云端断连。 */
        netconn_set_recvtimeout(g_conn, 1000);  // 接收超时1秒，主循环定期醒
        netconn_set_sendtimeout(g_conn, MQTT_IO_TIMEOUT_MS);  // 发送超时10秒

        err_t err = mqtt_connect();  // 非阻塞发起TCP连接
        if (err != ERR_OK)  // 连接失败
        {
            printf("[MQTT] TCP connect failed err=%d\r\n", (int)err);  // 打印连接失败
            goto conn_fail;  // 跳到连接失败处理
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
                if (err == ERR_OK)
                {
                    if (nb != NULL)
                    {
                        bool received = rx_ring_append(nb);
                        netbuf_delete(nb);
                        if (!received) goto conn_fail;
                    }

                    uint8_t mtype, mflags;
                    const uint8_t *mbody;
                    uint32_t mbody_len;
                    int flen = rx_ring_take_frame(&mtype, &mflags, &mbody, &mbody_len);
                    if (flen < 0) goto conn_fail;
                    if (flen > 0)
                    {
                        if (mtype == 2)  /* CONNACK = 0x20 >> 4 = 2 */  // 报文类型2是CONNACK
                        {
                            uint8_t rc = (mbody_len >= 2) ? mbody[1] : 0xFF;  // 取返回码，长度不足用0xFF
                            printf("[MQTT] CONNACK rc=%u (0=accepted 1=bad_proto 2=bad_cid 3=server_unavail 4=bad_auth 5=not_auth)\r\n",  // 打印返回码
                                   (unsigned)rc);
                            if (rc == 0)  // 返回码0连接接受
                            {
                                got_connack = true;  // 置已收到CONNACK
                                rx_ring_consume(flen);  // 消费这一帧
                                break;  // 跳出等待循环
                            }
                            else  // 返回码非0，连接被拒
                            {
                                /* ★★原 bug：错误 CONNACK 被静默丢弃，继续等直到超时。
                                   修复：认证失败直接退出，不浪费时间 */
                                printf("[MQTT] CONNECT rejected by broker (rc=%u)\r\n", (unsigned)rc);  // 打印被拒
                                rx_ring_consume(flen);  // 消费这一帧
                                goto conn_fail;  // 跳到连接失败
                            }
                        }
                        printf("[MQTT] unexpected msg type=%u (waiting CONNACK)\r\n", (unsigned)mtype);  // 非CONNACK打印意外类型
                        rx_ring_consume(flen);  // 消费掉
                    }
                }
                else  // 接收返回错误
                {
                    /* ★★调试：打印 netconn_recv 返回的错误码，判断是超时还是连接关闭 */
                    if (err != ERR_TIMEOUT)  // 不是超时
                        printf("[MQTT] recv err=%d (timeout=%d closed=%d)\r\n",  // 打印错误码
                               (int)err, (int)ERR_TIMEOUT, (int)ERR_CLSD);
                    if (nb != NULL) netbuf_delete(nb);  // 删掉netbuf
                    if (err != ERR_TIMEOUT)  // 不是超时就是断连
                    {
                        goto conn_fail;  // 跳到连接失败
                    }
                }
            }
            if (!got_connack)  // 等到超时都没收到CONNACK
            {
                printf("[MQTT] CONNACK timeout (%ums, no MQTT data received)\r\n",  // 打印超时
                       (unsigned)MQTT_CONNACK_TIMEOUT_MS);
                goto conn_fail;  // 跳到连接失败
            }
        }
        printf("[MQTT] connected!\r\n");  // 打印连接成功

        /* ---- 5. 发 SUBSCRIBE（commands + acks）+ 等 SUBACK ---- */
        pkt_len = mqtt_build_subscribe();  // 组订阅报文
        err = mqtt_write(tx_buf, pkt_len);  // 发送订阅
        if (err != ERR_OK) goto conn_fail;  // 发送失败跳到连接失败

        {
            bool got_suback = false;  // 是否收到SUBACK
            bool suback_ok = false;  // 订阅是否成功
            uint32_t t0 = osKernelGetTickCount();  // 记录开始时间
            while ((osKernelGetTickCount() - t0) < MQTT_SUBACK_TIMEOUT_MS)  // 等SUBACK，最多超时时间
            {
                struct netbuf *nb;  // 接收缓冲
                err = mqtt_receive(&nb);  // 收数据
                if (err == ERR_OK)  // 收到
                {
                    if (nb != NULL)  // 有数据
                    {
                        bool received = rx_ring_append(nb);  // 拼到环缓冲
                        netbuf_delete(nb);  // 删netbuf
                        if (!received) goto conn_fail;  // 拼失败跳连接失败
                    }

                    uint8_t mtype, mflags;  // 报文类型标志
                    const uint8_t *mbody;  // 载荷
                    uint32_t mbody_len;  // 载荷长度
                    int flen = rx_ring_take_frame(&mtype, &mflags, &mbody, &mbody_len);  // 取帧
                    if (flen < 0) goto conn_fail;  // 坏帧跳连接失败
                    if (flen > 0)  // 有完整帧
                    {
                        if (mtype == 9)   /* SUBACK */  // 类型9是SUBACK
                        {
                            got_suback = true;  // 置已收到
                            /* SUBACK: PacketID(2) + return code(s)
                             * 订阅了 2 个 topic，应有 2 个 return code
                             * 0x00=QoS0成功 0x01=QoS1成功 0x80=失败
                             * 设计 5.1: 检查两个 SUBACK 均成功才业务在线 */
                            if (mbody_len >= 4)  // 至少有2字节ID+2个返回码
                            {
                                if (mbody[2] != 0x80 && mbody[3] != 0x80)  // 两个返回码都不是失败0x80
                                {
                                    suback_ok = true;  // 订阅成功
                                }
                                else  // 有失败
                                {
                                    printf("[MQTT] SUBACK rejected: rc0=0x%02X rc1=0x%02X\r\n",  // 打印被拒
                                           mbody[2], mbody[3]);
                                }
                            }
                            else if (mbody_len >= 3 && mbody[2] != 0x80)  // 只有一个返回码且成功
                            {
                                /* 只有一个 return code（某些 broker） */
                                suback_ok = true;  // 订阅成功
                            }
                            rx_ring_consume(flen);  // 消费这一帧
                            break;  // 跳出等待
                        }
                        rx_ring_consume(flen);  // 非SUBACK消费掉
                    }
                }
                else if (nb != NULL)  // 接收失败但有netbuf
                {
                    netbuf_delete(nb);  // 删掉
                }
                if (err != ERR_OK && err != ERR_TIMEOUT) goto conn_fail;  // 非超时错误跳连接失败
            }
            if (!got_suback || !suback_ok)  // 没收到或订阅失败
            {
                printf("[MQTT] SUBACK failed/timeout, disconnect\r\n");  // 打印失败
                goto conn_fail;  // 跳连接失败
            }
            printf("[MQTT] subscribed commands + acks\r\n");  // 打印订阅成功
        }

        /* ---- 6. 上报 online（retain=true）+ boot 事件 ---- */
        g_connected = true;  // 置已连接，业务在线
        g_backoff_attempt = 0;   // 连接成功，重置退避次数
        {
            const char *online = "online";  // online状态字符串
            /* online 状态 retain=true（设计 5.1: 状态 retain=true） */
            if (!mqtt_lock_tx()) goto conn_fail;  // 拿发送锁
            size_t plen = mqtt_build_publish(g_topic_status, (const uint8_t *)online, 6, true, 0, 0, false);  // 组online报文，retain=true
            err = mqtt_write(tx_buf, plen);  // 发送online
            osMutexRelease(g_tx_mutex);  // 释放锁
            if (err != ERR_OK) goto conn_fail;  // 失败跳连接失败

            MqttClient_ReportEvent("boot", "gateway started");  // 上报boot事件
            printf("[MQTT] online + boot event published, entering main loop\r\n");  // 打印进入主循环
        }

        /* ---- 7. 主循环：收消息 + 心跳 + 补传 ---- */
        {
            uint32_t last_ping = osKernelGetTickCount();  // 上次心跳时间
            uint32_t last_replay = 0;  // 上次补传时间

            while (g_connected)  // 连接保持就循环
            {
                mqtt_pause_checkpoint();  // 检查暂停状态
                // 有在途批次就及时推进，持续收包也不能饿死发送或确认处理。
                if (g_fetch_in_flight || g_fetch_resp.count != 0U ||  // 有在途或本批未处理完
                    (osKernelGetTickCount() - last_replay) >= MQTT_REPLAY_INTERVAL_MS)  // 或到补传周期
                {
                    mqtt_replay_pump();  // 跑补传驱动
                    last_replay = osKernelGetTickCount();  // 更新补传时间
                }
                mqtt_qos1_check_resend();  // QoS1超时重发检查
                if (!g_connected) break;  // 重发检查中可能断连，跳出
                struct netbuf *nb;  // 接收缓冲
                err = mqtt_receive(&nb);  // 收数据，最多1秒

                if (err == ERR_TIMEOUT)  // 1秒没数据，超时
                {
                    /* 低频心跳：每 10 秒确认主循环在跑、recv 超时生效 */
                    static uint32_t last_alive = 0;  // 上次打印活跃时间
                    uint32_t now_tick = osKernelGetTickCount();  // 当前tick
                    if (now_tick - last_alive >= 10000U)  // 距上次打印超10秒
                    {
                        printf("[MQTT] main loop alive (keepalive=%us)\r\n", (unsigned)g_cfg.keepalive_sec);  // 打印主循环存活
                        last_alive = now_tick;  // 更新打印时间
                    }
                    /* 心跳：超过 keepalive 的 80% 没发数据就发 PINGREQ */
                    if ((osKernelGetTickCount() - last_ping) >= (uint32_t)(g_cfg.keepalive_sec * 800))  // 30秒的80%=24秒
                    {
                        if (!mqtt_lock_tx()) break;  // 拿发送锁失败跳出
                        size_t plen = mqtt_build_pingreq();  // 组心跳请求
                        err_t werr = mqtt_write(tx_buf, plen);  // 发心跳
                        osMutexRelease(g_tx_mutex);  // 释放锁
                        if (werr != ERR_OK)  // 心跳发送失败
                        {
                            printf("[MQTT] ping write fail, disconnect\r\n");  // 打印失败
                            break;  // 跳出主循环断连
                        }
                        last_ping = osKernelGetTickCount();  // 更新心跳时间
                    }

                    continue;  // 超时继续下一轮
                }

                if (err != ERR_OK)  // 收到非超时错误
                {
                    printf("[MQTT] read err=%d, disconnect\r\n", (int)err);  // 打印读错误
                    break;  // 跳出断连
                }

                /* 追加到 rx_ring，循环切帧处理 */
                if (nb != NULL)  // 有数据
                {
                    bool received = rx_ring_append(nb);  // 拼到环缓冲
                    netbuf_delete(nb);  // 删netbuf
                    if (!received) goto conn_fail;  // 拼失败跳连接失败
                }

                for (;;)  // 循环切帧，处理缓冲里所有完整帧
                {
                    uint8_t mtype, mflags;  // 报文类型标志
                    const uint8_t *mbody;  // 载荷
                    uint32_t mbody_len;  // 载荷长度
                    int flen = rx_ring_take_frame(&mtype, &mflags, &mbody, &mbody_len);  // 取一个完整帧
                    if (flen < 0) goto conn_fail;  // 坏帧跳连接失败
                    if (flen == 0) break;  // 没有完整帧，跳出

                    if (mtype == 3)        /* PUBLISH：下行命令 / 业务回执 */  // 类型3是PUBLISH
                    {
                        printf("[MQTT] PUBLISH recv flags=0x%02X body=%lu\r\n",  // 打印收到PUBLISH
                               (unsigned)mflags, (unsigned long)mbody_len);
                        /* 根据 topic 分发：commands → 命令处理，acks → 回执处理 */
                        if (mbody_len >= 4)  // 至少2字节长度+2字节内容
                        {
                            uint16_t tlen = ((uint16_t)mbody[0] << 8) | mbody[1];  // 拼主题长度
                            if ((uint32_t)(2 + tlen) <= mbody_len)  // 长度合法
                            {
                                const char *topic = (const char *)&mbody[2];  // 主题起始
                                if (tlen == strlen(g_topic_commands) &&  // 是commands主题
                                    memcmp(topic, g_topic_commands, tlen) == 0)
                                {
                                    mqtt_handle_command(mbody, mbody_len, mflags);  // 处理下行命令
                                }
                                else if (tlen == strlen(g_topic_acks) &&  // 是acks主题
                                         memcmp(topic, g_topic_acks, tlen) == 0)
                                {
                                    mqtt_handle_ack(mbody, mbody_len);  // 处理业务回执
                                }
                            }
                        }
                    }
                    else if (mtype == 13)  /* PINGRESP */  // 类型13是心跳回复
                    {
                        /* 心跳回复，无需处理 */
                    }
                    else if (mtype == 4)  /* PUBACK: QoS1 PUBLISH 确认 */  // 类型4是PUBACK
                    {
                        if (mbody_len >= 2)  // 至少2字节报文ID
                        {
                            uint16_t pid = ((uint16_t)mbody[0] << 8) | mbody[1];  // 拼报文ID
                            mqtt_handle_puback(pid);  // 处理QoS1确认
                        }
                    }
                    else  // 其他类型
                    {
                        /* 其它类型忽略 */
                    }
                    rx_ring_consume(flen);  // 消费这一帧
                    if (!mqtt_health_progress()) goto health_fail;  // 喂狗
                }
            }
        }

        /* ---- 8. 断连清理 ---- */
        (void)mqtt_connection_delete();  // 删除连接
        rx_ring_len = 0;  // 清接收缓冲
        g_fetch_in_flight = false;  // 清在途标志
        g_replay_last_seq = 0;  // 清补传序号
        g_replay_last_tick = 0;  // 清补传时间
        g_fetch_resp.count = 0U;  // 清本批记录
        g_replay_sent_mask = 0U;  // 清已发送位图
        g_replay_ack_mask = 0U;  // 清已确认位图
        {
            uint32_t delay = mqtt_backoff_delay(g_backoff_attempt++);  // 算退避时间，次数加1
            printf("[MQTT] disconnected, reconnect in %lums (attempt=%lu)\r\n",  // 打印重连等待
                   delay, (unsigned long)(g_backoff_attempt - 1));
            if (!mqtt_delay(delay)) goto health_fail;  // 退避延时喂狗
        }
        continue;  // 继续主循环重新连接

    conn_fail:  // 连接失败跳转点
        (void)mqtt_connection_delete();  // 删连接
        rx_ring_len = 0;  // 清接收缓冲
        {
            uint32_t delay = mqtt_backoff_delay(g_backoff_attempt++);  // 算退避时间
            printf("[MQTT] connect failed, retry in %lums (attempt=%lu)\r\n",  // 打印重试等待
                   delay, (unsigned long)(g_backoff_attempt - 1));
            if (!mqtt_delay(delay)) goto health_fail;  // 退避延时喂狗
        }
    }

health_fail:  // 健康失败跳转点
    taskENTER_CRITICAL();  // 关中断
    g_pause_acknowledged = false;  // 清暂停确认
    taskEXIT_CRITICAL();  // 开中断
    g_connected = false;  // 置断开
    Monitor_ReportFatal(APP_MODULE_MQTT);  // 上报致命错误
    osThreadExit();  // 退出本任务
}
