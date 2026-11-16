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
#include "mqtt_client.h"      // 本模块头文件，对外函数声明

#include <stdio.h>           // printf打印
#include <string.h>          // memcpy、strlen、strstr字符串操作
#include <stdlib.h>          // atoi转数字、rand随机数，重连抖动用
#include <stdbool.h>         // bool布尔类型，true/false

#include "cmsis_os2.h"       // FreeRTOS CMSIS V2封装，任务、延时
#include "app_monitor.h"     // 应用状态监控，上报运行状态
#include "upgrade_quiesce.h" // OTA升级静默，升级时暂停业务
#include "stm32f4xx_hal.h"   // HAL库，硬件操作
#include "FreeRTOS.h"        // FreeRTOS核心
#include "task.h"            // 任务相关API
#include "lwip/opt.h"        // LwIP配置，第一个必须包含
#include "lwip/api.h"        // netconn socket封装API，TCP连接用
#include "lwip/netbuf.h"     // netbuf收包缓冲
#include "lwip/ip_addr.h"    // IP地址结构
#include "lwip/netif.h"      // 网卡接口，查网卡状态、IP

#include "app_config.h"      // 读全局配置，MQTT参数从这来
#include "ethernet.h"        // 等待以太网链路就绪
#include "mb_api.h"          // Modbus接口，下行写寄存器注入
#include "storage_m2.h"      // M2缓存队列，离线补传、回执回收
#include "storage_record.h"  // 记录结构，遥测数据类型
#include "storage_seq.h"     // 序列号分配，报文ID
#include "storage_time.h"    // 读RTC时间，Unix时间戳签名用
#include "lwip/dns.h"        // DNS域名解析，云平台域名转IP
#include "hmac_sha256.h"     // HMAC-SHA256，百度云签名用
#if GATEWAY_ENABLE_M2                       // M2模块开关，开了才引入OTA头
#include "network_upgrade.h" // OTA升级通知，收到升级报文分流
#include "config_task.h"     // 配置维护模块，收到配置变更命令分流给Config处理
#endif                                       // 条件编译结束

// 上云模式开关，1连百度云IoT，0连本地mosquitto，改这个宏就能切换
#define MQTT_USE_BAIDU      1 // 上云模式开关，1百度云0本地mosquitto

#if MQTT_USE_BAIDU                          // 百度云模式才定义三元组
// 百度云三元组，控制台设备详情页复制，1883明文免TLS，省RAM不用加密
#define BAIDU_IOTCORE_ID    "aeklwww"           // IoT实例ID
#define BAIDU_DEVICE_KEY    "stm32_gw_01"       // 设备key，设备名
#define BAIDU_DEVICE_SECRET "YOUR_DEVICE_SECRET" // 设备密钥，签名用，占位符
#define BAIDU_REGION        "gz"                 // 地域，广州
#endif                                       // 条件编译结束

// 默认配置，AppConfig没就绪时回退用，本地模式生效，百度云模式不用这些
#define MQTT_DEF_BROKER_IP0   192       // broker IP第一段
#define MQTT_DEF_BROKER_IP1   168       // 第二段
#define MQTT_DEF_BROKER_IP2   2         // 第三段
#define MQTT_DEF_BROKER_IP3   45        // 第四段
#define MQTT_DEF_BROKER_PORT  1883      // MQTT默认端口
#define MQTT_DEF_CLIENT_ID    "mb2mqtt_001" // 默认客户端ID
#define MQTT_DEF_KEEPALIVE    30        // 心跳间隔秒，30秒没数据发心跳
#define MQTT_SUBACK_TIMEOUT_MS 2000     // 等订阅确认超时2秒
#define MQTT_CONNACK_TIMEOUT_MS 5000    // 等连接确认超时5秒，给云端留时间
#define MQTT_REPLAY_INTERVAL_MS 3000    // 补传任务调度间隔3秒
#define MQTT_REPLAY_RESEND_MS  30000    // 回执没到重发间隔30秒
#define MQTT_MAX_PAYLOAD       1024     // 最大载荷1024字节，RAM不够从4096缩的，超长直接拒绝
// 收发缓冲，载荷加MQTT头开销，留200余量，头包含固定头、topic、报文ID
#define MQTT_TX_BUF_SIZE       (MQTT_MAX_PAYLOAD + 200) // 发送缓冲
#define MQTT_RX_BUF_SIZE       (MQTT_MAX_PAYLOAD + 200) // 接收缓冲
#define MQTT_IO_TIMEOUT_MS    10000U    // 网络读写超时10秒
#define MQTT_TX_WAIT_MS       12000U    // 发送等待超时12秒
#define MQTT_HEALTH_MARGIN_MS  1000U    // 健康检查余量1秒

// QoS1未确认队列，发了QoS1报文等PUBACK，30秒没收到就重发
#define MQTT_QOS1_PENDING_MAX   2     // 同时最多2条未确认，RAM不够
#define MQTT_QOS1_PAYLOAD_MAX   512   // 每条缓存载荷最大512字节

// 单条未确认报文的结构，保存完整报文等重发
typedef struct { // QoS1待确认结构定义开始
    bool        in_use;               // 这个槽位有没有在用
    uint16_t    packet_id;            // 报文ID，PUBACK靠它匹配
    uint32_t    send_tick;            // 发送时刻，算超时用
    bool        retain;               // retain标志，重发要保持
    char        topic[96];            // 主题，重发要用
    uint8_t     payload[MQTT_QOS1_PAYLOAD_MAX]; // 载荷缓存
    size_t      payload_len;          // 载荷长度
} qos1_pending_t; // 别名qos1_pending_t定义结束

// 放CCMRAM不占普通SRAM，2条共约1.2KB，CCMRAM只能CPU访问不能DMA
static qos1_pending_t s_qos1_pending[MQTT_QOS1_PENDING_MAX] __attribute__((section(".bss.ccmram"))); // QoS1待确认数组放CCMRAM不占SRAM

// 重连退避表，断网后等待时间翻倍，1/2/4/8/16/32/60秒，再加20%随机抖动防雪崩
static const uint32_t g_backoff_table[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000}; // 重连退避表1到60秒翻倍
#define BACKOFF_TABLE_LEN  (sizeof(g_backoff_table) / sizeof(g_backoff_table[0])) // 表长度

// 子主题名，设备发遥测、事件、状态，订阅命令、回执，还有命令应答和OTA状态
#define MQTT_TOPIC_STATUS     "status"      // 在线状态主题
#define MQTT_TOPIC_EVENTS     "events"      // 事件告警主题
#define MQTT_TOPIC_TELEMETRY  "telemetry"   // 周期遥测主题
#define MQTT_TOPIC_COMMANDS   "commands"    // 下行命令主题，设备订阅
#define MQTT_TOPIC_ACKS       "acks"        // 业务回执主题，设备订阅
#define MQTT_TOPIC_CMD_ACK    "cmd_ack"     // 命令执行回执，设备发布
#define MQTT_TOPIC_OTA_STATUS "ota_status"  // OTA升级状态，设备发布

// 运行时配置，初始化时从AppConfig加载，DNS解析后填IP
typedef struct // 运行时配置结构定义开始
{
    char     broker_host[64];    // broker域名或IP字符串
    uint16_t broker_port;        // 端口
    ip_addr_t broker_ip;         // DNS解析后的IP，连接用
    bool     broker_ip_valid;    // DNS成功标志，没解析成功不能连
    char     client_id[48];      // 客户端ID，百度云就是设备key
    char     username[96];       // 用户名，百度云签名格式长所以加宽
    char     password[72];       // 密码，百度云是64位SHA256 hex
    uint16_t keepalive_sec;      // 心跳秒数
} mqtt_runtime_cfg_t; // 别名mqtt_runtime_cfg_t定义结束

static mqtt_runtime_cfg_t g_cfg; // 全局运行时配置，单例

// 完整主题缓冲，初始化时拼好前缀加子主题，不用每次发包都拼
static char g_topic_status[96]; // 在线状态主题缓冲
static char g_topic_events[96]; // 事件告警主题缓冲
static char g_topic_telemetry[96]; // 周期遥测主题缓冲
static char g_topic_commands[96]; // 下行命令主题缓冲
static char g_topic_acks[96]; // 业务回执主题缓冲
static char g_topic_cmd_ack[96]; // 命令应答主题缓冲
static char g_topic_ota_status[96]; // OTA状态主题缓冲

// 收发缓冲放CCMRAM，不占普通SRAM，CPU访问不用DMA所以安全，从4KB缩到1.2KB
static uint8_t tx_buf[MQTT_TX_BUF_SIZE]  __attribute__((section(".bss.ccmram"))); // 发送组包缓冲
static uint8_t rx_ring[MQTT_RX_BUF_SIZE] __attribute__((section(".bss.ccmram"))); // 接收拼包缓冲
static uint16_t rx_ring_len = 0;   // 接收缓冲当前有效字节数，TCP可能分包

// 下行命令载荷缓冲，拷出来加0结尾方便字符串解析
static char cmd_payload_buf[MQTT_MAX_PAYLOAD + 16] __attribute__((section(".bss.ccmram"))); // 下行命令载荷缓冲放CCMRAM

// 业务回执载荷缓冲，解析record_ids用
static char ack_payload_buf[MQTT_MAX_PAYLOAD + 16] __attribute__((section(".bss.ccmram"))); // 业务回执载荷缓冲放CCMRAM

// 全局运行状态，static限制只在本文件可见
static struct netconn *g_conn = NULL;      // TCP连接句柄，netconn类型
static volatile bool   g_connected = false; // MQTT是否已连接，volatile防优化
static volatile bool   g_connect_done;     // 连接握手完成标志
static osThreadId_t    g_mqtt_thread;      // 本任务句柄
static volatile bool g_pause_requested;    // 暂停请求标志，OTA时用
static bool g_pause_acknowledged;          // 暂停已应答标志

static void mqtt_pause_checkpoint(void);   // 暂停检查点前向声明

// 发送互斥锁，多个任务都可能发消息，保护发送缓冲和连接，防止并发踩内存
static osMutexId_t     g_tx_mutex; // 发送互斥锁句柄
static const osMutexAttr_t g_tx_mutex_attr = { .name = "mqtt_tx" }; // 锁属性：名字mqtt_tx

// 下行写寄存器同步信号量，等Modbus任务写完返回结果
static osSemaphoreId_t g_write_sem; // 写寄存器同步信号量句柄
static mb_errot_t      g_write_err;        // 写寄存器错误码

// 下一个MQTT报文ID，从1开始，QoS1、订阅都需要，不能为0
static uint16_t g_next_packet_id = 1; // 下一个MQTT报文ID从1开始
static uint16_t g_boot_result_packet_id;   // 启动结果报文ID
static bool g_boot_result_acked;           // 启动结果是否已收到PUBACK

static uint32_t g_backoff_attempt = 0;     // 重连失败次数，成功后清零，用来查退避表

// 补传追踪，记录上次补传最大序列号和时间，30秒没回执就重发
static uint64_t g_replay_last_seq = 0; // 上次补传最大序列号
static uint32_t g_replay_last_tick = 0; // 上次补传时间tick

// 函数：mqtt_health_progress —— 上报健康进展
// 功能：告诉监控模块“MQTT任务还活着、在正常干活”，相当于喂狗，防止任务被监控判定卡死而重启；只有MQTT自己的任务调用有效，别的任务调用直接返回真
// 参数：无
// 返回：bool，健康登记成功返回真；登记失败（如监控模块异常）返回假，调用方失败就走健康失败流程
static bool mqtt_health_progress(void)       // 喂健康看门狗函数，返回真代表任务正常
{
    if (osThreadGetId() != g_mqtt_thread)    // 判断当前运行线程是不是MQTT自己的线程
        return true;                         // 别的线程调用不用喂MQTT看门狗，直接返回真
    if (Monitor_ReportProgress(APP_MODULE_MQTT, true)) // 向监控模块上报我还在正常运行
        return true;                         // 上报成功，看门狗重新计时，返回真
    Monitor_ReportFatal(APP_MODULE_MQTT);    // 上报失败，标记MQTT模块致命故障
    return false;                            // 返回假，任务马上要退出
}

// 函数：mqtt_health_wait —— 阻塞前登记等待时长
// 功能：准备进入一段阻塞等待（等锁/等事件）之前调用，预先告诉监控模块“接下来最多等多少毫秒”，这段时间内不算卡死，防止正常等待被看门狗误判
// 参数：timeout_ms —— 本次预计最长等待的毫秒数，如等发送锁、等CONNACK的10秒
// 返回：bool，登记成功返回真；失败返回假，调用方应放弃等待直接走失败流程
static bool mqtt_health_wait(uint32_t timeout_ms) // 阻塞前告诉监控我要等多久，不算卡死
{
    if (osThreadGetId() != g_mqtt_thread)         // 判断是不是MQTT自己的线程
        return true;                              // 别的线程不用登记，直接返回真
    if (Monitor_BeginWait(APP_MODULE_MQTT, timeout_ms + MQTT_HEALTH_MARGIN_MS)) // 登记等待时间加余量
        return true;                              // 登记成功，等待期间看门狗不会误判卡死
    Monitor_ReportFatal(APP_MODULE_MQTT);         // 登记失败，上报致命错误
    return false;                                 // 返回假
}

// 函数：mqtt_delay —— 带健康监控的分段延时
// 功能：延时指定毫秒数但不一次性长睡，而是分小片段轮询睡眠，期间持续向监控模块上报进展，防止重连退避这种几十秒的长延时被误判成卡死
// 参数：delay_ms —— 需要延时的总毫秒数，例如重连退避最长72秒
// 返回：bool，全程健康上报成功返回真；中途健康上报失败返回假，调用方应中止延时走健康失败流程
static bool mqtt_delay(uint32_t delay_ms)  // 带健康监控的延时，不一次性长睡
{
    uint32_t started_ms = HAL_GetTick();   // 记录延时开始的系统tick毫秒数
    for (;;)                               // 无限循环，直到延时够了或者出错
    {
        mqtt_pause_checkpoint();           // 检查暂停状态，处理在途消息
        uint32_t elapsed = (uint32_t)(HAL_GetTick() - started_ms); // 算已经睡了多久
        if (elapsed >= delay_ms)           // 已经睡够要求的时间
            return true;                   // 延时成功返回真
        uint32_t remaining = delay_ms - elapsed; // 算还剩多少毫秒要等
        uint32_t step_ms = remaining < 1000U ? remaining : 1000U; // 最多一次睡1秒，剩多少睡多少
        if (!mqtt_health_wait(step_ms) || osDelay(pdMS_TO_TICKS(step_ms)) != osOK) // 先登记再睡，毫秒转tick
            return false;                  // 健康登记失败或者延时出错，返回假
        if (!mqtt_health_progress())       // 睡完喂看门狗
            return false;                  // 喂狗失败返回假
    }
}

// 函数：mqtt_lock_tx —— 拿发送互斥锁
// 功能：发送MQTT报文前先抢全局互斥锁，保证同一时刻只有一个任务在用发送缓冲tx_buf和网络连接，防止并发写把报文踩花
// 参数：无（锁句柄在全局g_tx_mutex里）
// 返回：bool，拿到锁返回真（用完必须释放）；健康登记失败或拿锁失败返回假，此时不能发送
static bool mqtt_lock_tx(void)             // 拿发送互斥锁，保护全局发送缓冲，防止多任务同时发
{
    if (!mqtt_health_wait(MQTT_TX_WAIT_MS))// 先登记等待锁的时间
        return false;                      // 健康检查失败直接返回假
    osStatus_t status = osMutexAcquire(g_tx_mutex, pdMS_TO_TICKS(MQTT_TX_WAIT_MS)); // 等锁，超时返回
    if (status != osOK && status != osErrorTimeout) // 既不是拿到锁也不是超时，是致命错误
    {
        if (osThreadGetId() == g_mqtt_thread) // 确认是MQTT自己的线程
            Monitor_ReportFatal(APP_MODULE_MQTT); // 上报致命错误
        return false;                      // 返回假
    }
    bool healthy = mqtt_health_progress(); // 拿到锁或者超时都喂一次狗
    if (status == osOK && healthy)         // 拿到锁而且任务健康
        return true;                       // 加锁成功返回真
    if (status == osOK)                    // 拿到锁但是任务不健康
        osMutexRelease(g_tx_mutex);        // 马上释放锁，不能带着锁退出
    return false;                          // 返回假
}

// 函数：mqtt_write —— 统一TCP发送封装
// 功能：把组好的MQTT报文通过netconn_write发给broker，用netconn拷贝发送模式确保数据先拷走再返回；发送失败会标记连接断开，防止后续继续在坏连接上发半包
// 参数：data —— 待发送报文的起始指针；len —— 报文字节数
// 返回：err_t，LwIP错误码，ERR_OK代表发送成功；其他值代表失败，调用方失败就走断线重连
static err_t mqtt_write(const void *data, size_t size) // 统一TCP发送函数，data是数据指针，size是长度
{
    if (!mqtt_health_wait(MQTT_IO_TIMEOUT_MS))         // 先登记发送超时时间
    {
        g_connected = false;                           // 健康失败标记连接断开
        return ERR_ABRT;                               // 返回中止错误
    }
    err_t err = netconn_write(g_conn, data, size, NETCONN_COPY); // 发送，COPY让LwIP内部拷贝数据，不依赖原缓冲
    if (!mqtt_health_progress())                       // 发送完喂狗
        err = ERR_ABRT;                                // 喂狗失败强制改成中止错误
    if (err != ERR_OK)                                 // 发送出错
        g_connected = false;                           // 标记连接断开，后面要重连
    return err;                                        // 返回LwIP错误码
}

// 函数：mqtt_receive —— 统一TCP接收封装
// 功能：从网络连接收一个netbuf数据块，1秒收超时（前面netconn_set_recvtimeout设的），主循环靠这个超时周期性醒来干活（发心跳、跑补传）
// 参数：buffer —— 二级指针出参，收到数据时*buffer指向新netbuf，调用方用完必须netbuf_delete释放
// 返回：err_t，ERR_OK且*buffer非空代表收到数据；ERR_TIMEOUT代表1秒没数据属正常；其他错误码代表连接出问题要断线
static err_t mqtt_receive(struct netbuf **buffer) // 统一TCP接收函数，输出收到的netbuf
{
    *buffer = NULL;                               // 先把输出指针清空，防止野指针
    if (!mqtt_health_wait(1000U))                 // 登记1秒接收超时
        return ERR_ABRT;                          // 健康失败直接返回中止
    err_t err = netconn_recv(g_conn, buffer);     // 等数据，1秒超时就返回超时错误
    if (!mqtt_health_progress())                  // 收完喂狗
    {
        if (*buffer != NULL)                      // 健康失败但已经收到数据了
        {
            netbuf_delete(*buffer);               // 删掉收到的缓冲，避免内存泄漏
            *buffer = NULL;                       // 指针清空
        }
        g_connected = false;                      // 标记连接断开
        return ERR_ABRT;                          // 返回中止
    }
    return err;                                   // 成功或者超时都返回对应错误码
}

// 函数：mqtt_netconn_event —— LwIP netconn事件回调
// 功能：创建netconn时注册的回调，LwIP内部在网络事件（收数据、发送完成、连接建立）时自动调用；这里只用来置位“连接完成”标志g_connect_done，配合mqtt_connect把阻塞式连接结果变成事件通知
// 参数：conn —— 触发事件的连接句柄；event —— 事件类型枚举；size —— 事件附带数据长度，本函数没用到
// 返回：void，无返回值
static void mqtt_netconn_event(struct netconn *conn, enum netconn_evt event, u16_t size) // LwIP事件回调，LwIP内部自动调用
{
    (void)size;                                                                         // 没用到size参数，强转void消除编译警告
    if (conn == g_conn && (event == NETCONN_EVT_SENDPLUS || event == NETCONN_EVT_ERROR)) // 是当前连接，而且连接成功或出错
        g_connect_done = true;                                                          // 标记连接有结果了，唤醒等待循环
}

// 函数：mqtt_connect —— 发起TCP连接
// 功能：向broker的IP和端口发起TCP三次握手，最多等10秒；连接结果由netconn回调置g_connect_done标志再确认，整体非阻塞设计，失败不卡任务
// 参数：无（目标地址在全局g_cfg.broker_ip和broker_port里）
// 返回：err_t，ERR_OK代表TCP连接建立成功；其他错误码代表失败，调用方走退避重试
static err_t mqtt_connect(void)             // 非阻塞发起TCP连接，最多等10秒
{
    g_connect_done = false;                // 先清连接完成标志
    netconn_set_nonblocking(g_conn, 1);    // 设非阻塞模式，连接调用立刻返回进行中
    if (!mqtt_health_wait(1000U))          // 健康登记1秒
    {
        netconn_set_nonblocking(g_conn, 0);// 改回阻塞模式
        return ERR_TIMEOUT;                // 返回超时错误
    }
    uint32_t started_ms = HAL_GetTick();   // 记录连接开始时间
    err_t err = netconn_connect(g_conn, &g_cfg.broker_ip, g_cfg.broker_port); // 发起连接，返回进行中
    if (!mqtt_health_progress())           // 喂狗
        err = ERR_TIMEOUT;                 // 健康失败强制超时
    while (err == ERR_INPROGRESS)          // 连接还在进行中就循环等
    {
        if (g_connect_done)                // 事件回调通知有结果了
        {
            err = netconn_err(g_conn);     // 读连接结果，成功就是ERR_OK
            break;                         // 跳出等待循环
        }
        uint32_t elapsed = (uint32_t)(HAL_GetTick() - started_ms); // 算已经等了多久
        if (elapsed >= MQTT_IO_TIMEOUT_MS) // 超过10秒超时
        {
            err = ERR_TIMEOUT;             // 标记超时
            break;                         // 跳出循环
        }
        uint32_t remaining = MQTT_IO_TIMEOUT_MS - elapsed; // 算剩余等待时间
        if (!mqtt_delay(remaining < 100U ? remaining : 100U)) // 最多100毫秒醒一次检查，防止等太久
        {
            err = ERR_TIMEOUT;             // 延时失败标记超时
            break;                         // 跳出循环
        }
    }
    netconn_set_nonblocking(g_conn, 0);    // 改回阻塞模式，后续收发用阻塞
    return err;                            // 返回连接结果
}

// 函数：mqtt_config_load —— 加载MQTT运行配置
// 功能：开机初始化调一次，把broker地址、端口、客户端ID、用户名密码、心跳间隔填到全局g_cfg结构体；百度云模式用宏定义三元组，本地模式用默认mosquitto参数，端口没填就用默认1883
// 参数：无（输出到全局结构体g_cfg）
// 返回：void，无返回值，配置无效时自动回退默认值不会失败
static void mqtt_config_load(void)       // 加载MQTT运行配置，开机初始化调一次
{
#if MQTT_USE_BAIDU                       // 条件编译，如果开了百度云模式，编译下面代码
    // 拼百度云broker完整域名，实例ID+区域+固定后缀
    snprintf(g_cfg.broker_host, sizeof(g_cfg.broker_host), // 拼百度云broker完整域名
             "%s.iot.%s.baidubce.com", BAIDU_IOTCORE_ID, BAIDU_REGION); // printf续行：实例ID+区域+后缀
    g_cfg.broker_port = 1883;            // MQTT明文端口固定1883
    g_cfg.broker_ip_valid = false;       // 还没DNS解析，IP无效，连接时才解析
    g_cfg.keepalive_sec = MQTT_DEF_KEEPALIVE; // 心跳间隔用默认值
#else                                    // 没开百度云，编译本地模式代码
    const app_config_t *cfg = AppConfig_Get(); // 拿全局配置结构体指针
    // 指针有效、配置魔数对、broker地址非空，才用Flash里的配置
    if (cfg != NULL && cfg->magic == CONFIG_MAGIC && cfg->mq_broker_host[0] != '\0') // 配置有效且地址非空才用
    {
        // 拷贝broker地址，留1字节放\0
        strncpy(g_cfg.broker_host, cfg->mq_broker_host, sizeof(g_cfg.broker_host)-1); // 拷broker地址留结尾
        g_cfg.broker_host[sizeof(g_cfg.broker_host)-1] = '\0'; // 强制字符串结尾
        // 端口非0用配置，0就用默认端口
        g_cfg.broker_port = cfg->mq_broker_port ? cfg->mq_broker_port : MQTT_DEF_BROKER_PORT; // 端口0就用默认1883
        // 拷贝客户端ID
        strncpy(g_cfg.client_id, cfg->mq_client_id, sizeof(g_cfg.client_id)-1); // 拷客户端ID
        g_cfg.client_id[sizeof(g_cfg.client_id)-1] = '\0'; // 强制字符串结尾
        // 拷贝用户名
        strncpy(g_cfg.username, cfg->mq_username, sizeof(g_cfg.username)-1); // 拷用户名
        g_cfg.username[sizeof(g_cfg.username)-1] = '\0'; // 强制字符串结尾
        // 拷贝密码
        strncpy(g_cfg.password, cfg->mq_password, sizeof(g_cfg.password)-1); // 拷密码
        g_cfg.password[sizeof(g_cfg.password)-1] = '\0'; // 强制字符串结尾
        g_cfg.keepalive_sec = 30;         // 本地模式固定30秒心跳
        g_cfg.broker_ip_valid = false;   // 统一走DNS，IP无效
        return;                          // 配置加载完成直接返回
    }
    // Flash配置没就绪，回退默认本地broker地址
    strncpy(g_cfg.broker_host, "192.168.2.45", sizeof(g_cfg.broker_host)-1); // 回退默认本地broker地址
    g_cfg.broker_host[sizeof(g_cfg.broker_host)-1] = '\0'; // 强制字符串结尾
    g_cfg.broker_port  = MQTT_DEF_BROKER_PORT; // 默认端口
    // 默认客户端ID
    strncpy(g_cfg.client_id, MQTT_DEF_CLIENT_ID, sizeof(g_cfg.client_id)-1); // 拷默认客户端ID
    g_cfg.client_id[sizeof(g_cfg.client_id)-1] = '\0'; // 强制字符串结尾
    g_cfg.username[0] = '\0';            // 默认无用户名
    g_cfg.password[0] = '\0';            // 默认无密码
    g_cfg.keepalive_sec = MQTT_DEF_KEEPALIVE; // 默认心跳
    g_cfg.broker_ip_valid = false;       // IP无效
#endif                                   // 条件编译结束
}

#if MQTT_USE_BAIDU                       // 百度云模式才编译这个函数

// 函数：compile_time_to_unix —— 编译时刻转Unix时间戳
// 功能：解析__DATE__和__TIME__宏里的编译时刻字符串，手动换算成UTC的Unix秒时间戳，给百度云HMAC签名的time字段用；板子没RTC也能拿到大致时间（烧录后误差只是编译到上电的间隔）
// 参数：无（读编译器内置宏__DATE__和__TIME__）
// 返回：uint32_t，编译时刻对应的Unix秒时间戳（UTC），换算含闰年判断和东八区减8小时
static uint32_t compile_time_to_unix(void) // 把编译时刻的本地时间转成UTC的Unix秒时间戳
{
    static const char *const months = "JanFebMarAprMayJunJulAugSepOctNovDec"; // 月份缩写连续字符串，每个3字符
    const char *mstr = __DATE__;           // 编译器内置宏，编译日期字符串Mmm dd yyyy
    const char *tstr = __TIME__;           // 编译器内置宏，编译时间字符串hh:mm:ss

    char mon3[4] = {0};                    // 存3字符月份缩写，第4字节固定\0
    mon3[0] = mstr[0]; mon3[1] = mstr[1]; mon3[2] = mstr[2]; // 取日期前3个字符
    const char *p = strstr(months, mon3);  // 在月份字符串里找缩写位置
    // 找到算月份，找不到默认1月，指针差除以3加1
    int month = p ? (int)((p - months) / 3 + 1) : 1; // 找月份缩写位置算月份

    int day = atoi(mstr + 4);              // 偏移4开始是日期数字，转整数
    int year = atoi(mstr + 7);             // 偏移7开始是年份数字，转整数

    int hour = atoi(tstr);                 // 开头是小时，转整数
    int min = atoi(tstr + 3);              // 偏移3开始分钟，转整数
    int sec = atoi(tstr + 6);              // 偏移6开始秒，转整数

    uint32_t days = 0;                     // 累加从1970-1-1到目标日期的总天数
    for (int y = 1970; y < year; y++)      // 遍历1970到去年的每一年
        // 能被4整除且不能被100整除，或者能被400整除就是闰年366，否则平年365
        days += ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 366 : 365; // 闰年366平年365

    // 每个月天数表，1月到12月，平年2月28
    static const int dom[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; // 每月天数表平年
    for (int m = 1; m < month; m++)        // 遍历今年1月到上个月
    {
        days += dom[m - 1];                // 加当月天数
        // 2月而且今年是闰年，多加1天
        if (m == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) // 闰年2月多加1天
            days += 1; // 补闰日
    }
    days += (uint32_t)(day - 1);           // 加当月已经过的天数，1号当天加0

    // 一天86400秒，小时3600，分钟60，北京UTC+8减8小时28800秒，得到UTC时间戳
    return days * 86400U + (uint32_t)(hour * 3600 + min * 60 + sec) - 28800U; // 总秒数减8小时转UTC
}

// 函数：baidu_build_auth —— 生成百度云连接认证信息
// 功能：每次连接broker前重算百度云IoT认证：用户名按“实例/设备/key/时间戳”格式拼，密码取设备密钥做HMAC-SHA256后转hex；时间戳传0让百度跳过校验（设备没RTC），算完存全局g_cfg供CONNECT报文用
// 参数：无（三元组来自宏BAIDU_IOTCORE_ID/BAIDU_DEVICE_KEY/BAIDU_DEVICE_SECRET，输出到g_cfg.username/password）
// 返回：void，无返回值，签名失败时密码为空，连接会被broker拒绝再走重试
static void baidu_build_auth(void)       // 生成百度云MQTT连接的用户名和密码，每次连接前重算
{
    const char *iotcore_id = BAIDU_IOTCORE_ID; // 拿宏定义的百度云实例ID
    const char *dk = BAIDU_DEVICE_KEY;   // 拿宏定义的设备Key，设备唯一标识
    const char *ds = BAIDU_DEVICE_SECRET;// 拿宏定义的设备密钥，签名用，不能泄露

    uint32_t ts = 0;                     // 时间戳固定传0，百度云允许跳过时间校验，避免时区问题

    // 拼用户名，百度云固定格式，竖线分隔四个字段
    snprintf(g_cfg.username, sizeof(g_cfg.username), // 拼用户名四字段竖线分隔
             "thingidp@%s|%s|%lu|SHA256", // printf续行：格式串
             iotcore_id, dk, (unsigned long)ts); // printf续行：实例设备key时间戳

    char sign_str[128];                  // 临时数组存待签名的字符串，128字节足够
    // 拼待签名串，设备key&时间戳&SHA256设备密钥，n返回实际长度
    int n = snprintf(sign_str, sizeof(sign_str), // 拼待签名串
                     "%s&%lu&SHA256%s", dk, (unsigned long)ts, ds); // printf续行：key时间戳算法密钥

    // 对待签名串算SHA256，输出64字符小写hex字符串，就是连接密码
    sha256_hex((const uint8_t *)sign_str, (size_t)n, g_cfg.password); // SHA256转hex当密码

    // 客户端ID直接用设备Key
    strncpy(g_cfg.client_id, dk, sizeof(g_cfg.client_id)-1); // 客户端ID用设备key
    g_cfg.client_id[sizeof(g_cfg.client_id)-1] = '\0'; // 强制字符串结尾

    // 打印认证信息调试，不打印密钥
    printf("[MQTT] Baidu auth: cid=%s ts=%lu\r\n", // 打印认证信息不带密钥
           g_cfg.client_id, (unsigned long)ts); // printf续行：ID和时间戳
}
#endif                                   // 百度云条件编译结束

const char *MqttClient_TopicTelemetry(void) // 对外接口，返回遥测完整主题字符串
{
    return g_topic_telemetry;                // 返回全局拼好的遥测主题，遥测任务发数据用
}

const char *MqttClient_TopicOtaStatus(void)  // 对外接口，返回OTA状态上报完整主题
{
    return g_topic_ota_status;               // 返回全局拼好的OTA主题，升级模块上报状态用
}

// 函数：mqtt_topics_build —— 拼7个MQTT完整主题
// 功能：初始化时把status/events/telemetry/commands/acks/cmd_ack/ota_status七个主题拼成完整字符串存全局数组；百度云模式前缀是$iot/设备key/...，本地模式前缀是gateway/...；拼好后面收发包按主题名分发不用反复拼
// 参数：无（输出到全局g_topic_xxx七个数组）
// 返回：void，无返回值，纯字符串拼接不会失败
static void mqtt_topics_build(void)      // 拼7个完整MQTT主题，初始化一次拼好，后面直接用
{
#if MQTT_USE_BAIDU                       // 百度云模式编译下面分支
    const char *dk = g_cfg.client_id;    // 客户端ID就是设备key，简化变量名
    // 依次拼7个主题，百度云固定前缀$iot/设备key/user/，后面加子主题名
    snprintf(g_topic_status,    sizeof(g_topic_status),    "$iot/%s/user/%s", dk, MQTT_TOPIC_STATUS); // 拼 status 主题
    snprintf(g_topic_events,    sizeof(g_topic_events),    "$iot/%s/user/%s", dk, MQTT_TOPIC_EVENTS); // 拼 events 主题
    snprintf(g_topic_telemetry, sizeof(g_topic_telemetry), "$iot/%s/user/%s", dk, MQTT_TOPIC_TELEMETRY); // 拼 telemetry 主题
    snprintf(g_topic_commands,  sizeof(g_topic_commands),  "$iot/%s/user/%s", dk, MQTT_TOPIC_COMMANDS); // 拼 commands 主题
    snprintf(g_topic_acks,      sizeof(g_topic_acks),      "$iot/%s/user/%s", dk, MQTT_TOPIC_ACKS); // 拼 acks 主题
    snprintf(g_topic_cmd_ack,   sizeof(g_topic_cmd_ack),   "$iot/%s/user/%s", dk, MQTT_TOPIC_CMD_ACK); // 拼 cmd_ack 主题
    snprintf(g_topic_ota_status,sizeof(g_topic_ota_status),"$iot/%s/user/%s", dk, MQTT_TOPIC_OTA_STATUS); // 拼 ota_status 主题
#else                                    // 本地模式编译下面分支
    // 本地broker格式，客户端ID/子主题，没有固定前缀
    snprintf(g_topic_status,    sizeof(g_topic_status),    "%s/%s", g_cfg.client_id, MQTT_TOPIC_STATUS); // 本地模式拼 status
    snprintf(g_topic_events,    sizeof(g_topic_events),    "%s/%s", g_cfg.client_id, MQTT_TOPIC_EVENTS); // 本地模式拼 events
    snprintf(g_topic_telemetry, sizeof(g_topic_telemetry), "%s/%s", g_cfg.client_id, MQTT_TOPIC_TELEMETRY); // 本地模式拼 telemetry
    snprintf(g_topic_commands,  sizeof(g_topic_commands),  "%s/%s", g_cfg.client_id, MQTT_TOPIC_COMMANDS); // 本地模式拼 commands
    snprintf(g_topic_acks,      sizeof(g_topic_acks),      "%s/%s", g_cfg.client_id, MQTT_TOPIC_ACKS); // 本地模式拼 acks
    snprintf(g_topic_cmd_ack,   sizeof(g_topic_cmd_ack),   "%s/%s", g_cfg.client_id, MQTT_TOPIC_CMD_ACK); // 本地模式拼 cmd_ack
    snprintf(g_topic_ota_status,sizeof(g_topic_ota_status),"%s/%s", g_cfg.client_id, MQTT_TOPIC_OTA_STATUS); // 本地模式拼 ota_status
#endif                                   // 条件编译结束
}

// 函数：mqtt_encode_remaining_length —— 编码MQTT剩余长度
// 功能：把报文剩余长度按MQTT规范变长编码写进buf：每字节低7位有效、最高位是继续标志、最多4字节，能表示最大约268MB的报文；固定头第二字节起就是它
// 参数：buf —— 编码输出缓冲指针（一般指向tx_buf固定头之后）；value —— 要编码的剩余长度数值
// 返回：int，实际写入的字节数（1~4），调用方据此移动后续填充位置
static int mqtt_encode_remaining_length(uint8_t *buf, uint32_t value) // MQTT剩余长度变长编码，输出字节数
{
    int n = 0;                                                        // 记录已经写了几个字节
    do {                                                               // do-while先执行一次，至少写1字节
        uint8_t b = value & 0x7F;                                      // 取当前值低7位
        value >>= 7;                                                   // 右移7位，处理下一组
        if (value > 0) b |= 0x80;                                      // 还有数据，最高位置1标记后续还有字节
        buf[n++] = b;                                                  // 字节写到输出缓冲，长度加1
    } while (value > 0 && n < 4);                                      // 还有数据且最多4字节就继续
    return n;                                                          // 返回编码用了几个字节
}

// 函数：mqtt_decode_remaining_length —— 解码MQTT剩余长度
// 功能：收包时把固定头第二字节起的变长剩余长度还原成数值，是rx_ring_take_frame切帧的前提；会校验编码字节数不超4且数据够长，防止半包或脏数据算出错误长度
// 参数：data —— 待解码数据起始指针；avail —— 当前可用字节数；out_value —— 出参，解码出的剩余长度；out_bytes —— 出参，编码占了几个字节
// 返回：int，0代表解码成功；-1代表数据不完整或编码非法，此时应继续等后续数据
static int mqtt_decode_remaining_length(const uint8_t *data, uint16_t avail, uint32_t *out_value, int *out_bytes) // 解码剩余长度，0成功-1失败
{
    uint32_t value = 0;                                                                                        // 累加最终长度值
    int multiplier = 1;                                                                                        // 每级乘128，第一级乘1
    int i = 0;                                                                                                 // 当前读到第几个字节
    while (i < avail && i < 4)                                                                                 // 还有数据且最多4字节
    {
        uint8_t b = data[i];                                                                                   // 读当前字节
        value += (uint32_t)(b & 0x7F) * multiplier;                                                           // 低7位乘权重累加
        i++;                                                                                                   // 偏移加1
        if ((b & 0x80) == 0)                                                                                   // 最高位0，这是最后一个字节
        {
            *out_value = value;                                                                                // 输出最终长度
            *out_bytes = i;                                                                                    // 输出编码占了几个字节
            return 0;                                                                                          // 返回成功
        }
        multiplier *= 128;                                                                                     // 下一级权重乘128
    }
    return -1;                                                                                                 // 数据不完整，返回失败
}

// 函数：mqtt_backoff_delay —— 计算重连退避时长
// 功能：按重试次数查表算下次重连要等多久：基础序列1/2/4/8/16/32/60秒封顶，再叠加20%随机抖动（rand生成），防止大量设备同一时刻集体重连打爆broker
// 参数：attempt —— 第几次重连（从0开始），作为退避表下标
// 返回：uint32_t，本次应等待的毫秒数，最长约72秒
static uint32_t mqtt_backoff_delay(uint32_t attempt) // 计算重连等待毫秒数，带随机抖动
{
    uint32_t idx = attempt;                          // 第几次重连，作为表下标
    if (idx >= BACKOFF_TABLE_LEN) idx = BACKOFF_TABLE_LEN - 1; // 超过表长度，固定用最大60秒
    uint32_t base = g_backoff_table[idx];            // 查基础等待时间
    uint32_t jitter = base / 5;                      // 抖动范围是基础值的20%
    if (jitter > 0) jitter = (uint32_t)rand() % jitter; // 生成0到jitter之间的随机数
    return base + jitter;                            // 基础加随机抖动就是最终等待时间
}

// MQTT报文构造，都写入全局tx_buf，返回报文总长度

// 函数：mqtt_build_connect —— 组CONNECT连接报文
// 功能：拼MQTT第一个握手报文写入全局tx_buf：协议名MQTT 3.1.1、CleanSession开、带遗嘱LWT（掉线让broker代发通知）、心跳间隔、客户端ID、用户名密码；可变头和载荷先拼临时数组vh再整体拷入，最后编码剩余长度
// 参数：无（认证信息来自全局g_cfg，遗嘱主题和消息来自全局变量）
// 返回：size_t，组好的报文总长度，直接交给mqtt_write发送
static size_t mqtt_build_connect(void)     // 组CONNECT连接报文，返回报文总长度
{
    uint8_t vh[512];                       // 临时数组拼可变头和载荷，512字节足够
    size_t n = 0;                          // 临时数组当前写入偏移

    // 协议名，2字节长度0004，后面4字节MQTT
    vh[n++] = 0x00; vh[n++] = 0x04; // 协议名长度0004和MQTT前2字符
    vh[n++] = 'M'; vh[n++] = 'Q'; vh[n++] = 'T'; vh[n++] = 'T'; // 协议名后2字符
    vh[n++] = 0x04;                        // 协议级别4，对应MQTT 3.1.1

    bool has_auth = (g_cfg.username[0] != '\0'); // 用户名非空就代表需要认证
    // 基础标志：bit1 CleanSession=1，bit2遗嘱标志，bit5遗嘱retain=1
    uint8_t flags = 0x02 | 0x04 | 0x20; // 标志字：CleanSession+遗嘱+retain
    if (has_auth) flags |= 0x80 | 0x40;    // 有认证加bit7用户名、bit6密码标志
    vh[n++] = flags;                       // 标志字写入可变头

    // 心跳2字节大端，高字节在前低字节在后
    vh[n++] = (uint8_t)((g_cfg.keepalive_sec >> 8) & 0xFF); // 心跳高字节
    vh[n++] = (uint8_t)(g_cfg.keepalive_sec & 0xFF); // 心跳低字节

    size_t cidlen = strlen(g_cfg.client_id); // 算客户端ID字符串长度
    // 2字节大端长度
    vh[n++] = (uint8_t)((cidlen >> 8) & 0xFF); // 客户端ID长度高字节
    vh[n++] = (uint8_t)(cidlen & 0xFF); // 客户端ID长度低字节
    memcpy(&vh[n], g_cfg.client_id, cidlen); // 拷贝客户端ID字符串
    n += cidlen;                           // 偏移加长度

    const char *will_topic = g_topic_status; // 遗嘱主题就是status主题，掉线broker自动发
    size_t wtlen = strlen(will_topic);     // 遗嘱主题长度
    // 2字节大端长度
    vh[n++] = (uint8_t)((wtlen >> 8) & 0xFF); // 遗嘱主题长度高字节
    vh[n++] = (uint8_t)(wtlen & 0xFF); // 遗嘱主题长度低字节
    memcpy(&vh[n], will_topic, wtlen);     // 拷贝遗嘱主题
    n += wtlen;                            // 偏移加长度

    const char *will_msg = "offline";      // 遗嘱消息固定offline，掉线自动发
    size_t wmlen = strlen(will_msg);       // 遗嘱消息长度
    // 2字节大端长度
    vh[n++] = (uint8_t)((wmlen >> 8) & 0xFF); // 遗嘱消息长度高字节
    vh[n++] = (uint8_t)(wmlen & 0xFF); // 遗嘱消息长度低字节
    memcpy(&vh[n], will_msg, wmlen);       // 拷贝遗嘱消息
    n += wmlen;                            // 偏移加长度

    if (has_auth)                          // 有认证才加用户名密码
    {
        size_t ulen = strlen(g_cfg.username); // 用户名长度
        // 2字节大端长度
        vh[n++] = (uint8_t)((ulen >> 8) & 0xFF); // 用户名长度高字节
        vh[n++] = (uint8_t)(ulen & 0xFF); // 用户名长度低字节
        memcpy(&vh[n], g_cfg.username, ulen); // 拷贝用户名
        n += ulen;                         // 偏移加长度

        size_t plen = strlen(g_cfg.password); // 密码长度
        // 2字节大端长度
        vh[n++] = (uint8_t)((plen >> 8) & 0xFF); // 密码长度高字节
        vh[n++] = (uint8_t)(plen & 0xFF); // 密码长度低字节
        memcpy(&vh[n], g_cfg.password, plen); // 拷贝密码
        n += plen;                         // 偏移加长度
    }

    size_t off = 0;                        // 最终发送缓冲偏移
    tx_buf[off++] = 0x10;                  // 固定头高4位1=CONNECT，低4位0
    // 编码剩余长度，就是可变头加载荷的总长度n
    off += mqtt_encode_remaining_length(&tx_buf[off], (uint32_t)n); // 编码剩余长度写入固定头
    memcpy(&tx_buf[off], vh, n);           // 临时数组整个拷到发送缓冲
    return off + n;                        // 固定头+可变头+载荷，返回总长度
}

// 函数：mqtt_build_subscribe —— 组SUBSCRIBE订阅报文
// 功能：一次组订阅两个主题（commands下行命令+acks业务回执）的报文写入tx_buf：固定头、自增的报文ID、每个主题带长度前缀和QoS等级；broker对每个主题逐一用SUBACK返回码回应
// 参数：无（主题来自全局g_topic_commands和g_topic_acks）
// 返回：size_t，报文总长度，交给mqtt_write发送
static size_t mqtt_build_subscribe(void)  // 组SUBSCRIBE订阅报文，一次订阅两个主题
{
    uint8_t vh[160];                      // 临时数组拼可变头，160字节足够
    size_t n = 0;                         // 临时数组偏移

    // 先取当前ID再自增，订阅报文必须带报文ID
    uint16_t pid = g_next_packet_id++; // 取报文ID再自增
    if (g_next_packet_id == 0) g_next_packet_id = 1; // 16位溢出回1，ID不能为0
    // 2字节大端报文ID
    vh[n++] = (uint8_t)((pid >> 8) & 0xFF); // 报文ID高字节
    vh[n++] = (uint8_t)(pid & 0xFF); // 报文ID低字节

    size_t tlen1 = strlen(g_topic_commands); // 第一个commands主题长度
    // 2字节大端长度
    vh[n++] = (uint8_t)((tlen1 >> 8) & 0xFF); // commands主题长度高字节
    vh[n++] = (uint8_t)(tlen1 & 0xFF); // commands主题长度低字节
    memcpy(&vh[n], g_topic_commands, tlen1); // 拷贝主题字符串
    n += tlen1;                          // 偏移加长度
    vh[n++] = 0x00;                      // QoS级别0，最多发一次不确认

    size_t tlen2 = strlen(g_topic_acks);    // 第二个acks主题长度
    // 2字节大端长度
    vh[n++] = (uint8_t)((tlen2 >> 8) & 0xFF); // acks主题长度高字节
    vh[n++] = (uint8_t)(tlen2 & 0xFF); // acks主题长度低字节
    memcpy(&vh[n], g_topic_acks, tlen2);    // 拷贝主题字符串
    n += tlen2;                          // 偏移加长度
    vh[n++] = 0x00;                      // QoS级别0

    size_t off = 0;                      // 最终发送缓冲偏移
    tx_buf[off++] = 0x82;                // 高4位8=SUBSCRIBE，低4位固定0010
    // 编码剩余长度
    off += mqtt_encode_remaining_length(&tx_buf[off], (uint32_t)n); // 编码剩余长度
    memcpy(&tx_buf[off], vh, n);         // 拷贝可变头
    return off + n;                      // 返回报文总长度
}

// 函数：mqtt_build_publish —— 组PUBLISH发布报文
// 功能：通用发布组包函数写入tx_buf：固定头带retain/QoS/DUP标志位，可变头是主题名和QoS1时的报文ID，载荷原样拷贝；所有上行发布（遥测/事件/状态/OTA结果）都走这一个函数
// 参数：topic —— 主题字符串；payload —— 载荷指针（JSON或二进制）；plen —— 载荷长度；retain —— 保留标志（状态和遗嘱为真，业务数据为假）；qos —— QoS等级0或1；packet_id —— QoS1时的报文ID用于确认配对；dup —— 重发标志，真表示这是重发的旧包
// 返回：size_t，报文总长度；0代表组包失败（超缓冲），调用方应放弃发送
static size_t mqtt_build_publish(const char *topic, const uint8_t *payload, size_t plen, // 组PUBLISH报文函数：7个参数
                                 bool retain, uint8_t qos, uint16_t packet_id, bool dup) // 签名续行：标志QoS报文ID重发
{
    size_t tlen = strlen(topic);                                                         // 算主题字符串长度
    size_t remaining = 2 + tlen + plen;                                                  // 剩余长度：2字节主题长度+主题+载荷
    if (qos > 0) remaining += 2;                                                         // QoS1多2字节报文ID

    size_t off = 0;                                                                      // 发送缓冲偏移
    // 基础头高4位3=PUBLISH，最低位retain
    uint8_t header = 0x30 | (retain ? 0x01 : 0x00); // 头字节：PUBLISH+retain位
    if (qos == 1) header |= 0x02;                                                        // bit1置1，QoS1
    if (dup)      header |= 0x08;                                                        // bit3置1，DUP重发
    tx_buf[off++] = header;                                                              // 固定头写入
    off += mqtt_encode_remaining_length(&tx_buf[off], (uint32_t)remaining);             // 编码剩余长度
    // 2字节大端主题长度
    tx_buf[off++] = (uint8_t)((tlen >> 8) & 0xFF); // 主题长度高字节
    tx_buf[off++] = (uint8_t)(tlen & 0xFF); // 主题长度低字节
    memcpy(&tx_buf[off], topic, tlen);                                                   // 拷贝主题字符串
    off += tlen;                                                                         // 偏移加主题长度
    if (qos > 0)                                                                         // 只有QoS1才加报文ID
    {
        // 2字节大端报文ID
        tx_buf[off++] = (uint8_t)((packet_id >> 8) & 0xFF); // 报文ID高字节
        tx_buf[off++] = (uint8_t)(packet_id & 0xFF); // 报文ID低字节
    }
    memcpy(&tx_buf[off], payload, plen);                                                 // 最后拷贝载荷
    off += plen;                                                                         // 偏移加载荷长度
    return off;                                                                          // 返回报文总长度
}

// 函数：mqtt_build_pingreq —— 组心跳请求报文
// 功能：组MQTT PINGREQ保活报文写入tx_buf，固定就2字节没有可变头和载荷；主循环按心跳间隔80%周期性发送，防止broker判定设备空闲超时踢人
// 参数：无
// 返回：size_t，固定返回2（报文总长度）
static size_t mqtt_build_pingreq(void)    // 组心跳请求报文，固定2字节，没有载荷
{
    tx_buf[0] = 0xC0;                      // 高4位C=PINGREQ，低4位固定0
    tx_buf[1] = 0x00;                      // 剩余长度0，没有可变头和载荷
    return 2;                              // 报文总长度就是2字节
}

// 函数：rx_ring_append —— TCP分段追加拼包
// 功能：TCP是流式协议，一个MQTT报文可能分几个TCP段到达，本函数把新到的数据追加到全局接收环缓冲尾部；单段超容量或追加后放不下的整段丢弃并打日志，明确拒绝不截断
// 参数：data —— 新收到数据的起始指针；len —— 本段数据字节数
// 返回：void，无返回值，溢出时数据被丢弃但连接不断
static void rx_ring_append(const uint8_t *data, size_t len) // TCP分段追加拼包函数
{
    if (len >= sizeof(rx_ring))                                   // 单段数据比整个缓冲还大，肯定装不下
    {
        // 打印丢弃日志，明确是拒绝不是截断
        printf("[MQTT] drop oversize seg %u > ring %u (rejected, not truncated)\r\n", // 打印超大段丢弃日志
               (unsigned)len, (unsigned)sizeof(rx_ring)); // printf续行：段长和缓冲容量
        rx_ring_len = 0;                                          // 清空整个缓冲
        return;                                                   // 直接返回，不拷贝
    }
    if (rx_ring_len + len > sizeof(rx_ring))                      // 已有数据加新段会溢出
    {
        // 打印溢出日志，说明前帧不完整
        printf("[MQTT] drop overflow %u+%u > ring %u (rejected, not truncated)\r\n", // 打印溢出丢弃日志
               (unsigned)rx_ring_len, (unsigned)len, (unsigned)sizeof(rx_ring)); // printf续行：已有加新段超容量
        rx_ring_len = 0;                                          // 清空旧数据，新段可能是新帧开头
    }
    // 新数据拷贝到缓冲已有数据的尾部
    memcpy(rx_ring + rx_ring_len, data, len); // 新数据拷到缓冲尾部
    rx_ring_len += len;                                           // 更新缓冲总长度
}

// 函数：rx_ring_take_frame —— 从缓冲取一个完整MQTT帧
// 功能：检查接收环缓冲里有没有一个完整的MQTT报文：先解码剩余长度算出帧总长，够一帧就输出报文类型、标志、载荷位置；不够返回0等后续数据；超大帧直接丢弃
// 参数：out_type —— 出参，报文类型（固定头高4位）；out_flags —— 出参，标志位（低4位）；out_body —— 出参，载荷起始指针；out_body_len —— 出参，载荷长度
// 返回：size_t，完整帧的总长度（含固定头）；0代表还没有完整帧，调用方应继续收数据
static size_t rx_ring_take_frame(uint8_t *out_type, uint8_t *out_flags, // 取完整帧函数：4个出参
                                 const uint8_t **out_body, uint32_t *out_body_len) // 签名续行：载荷指针和长度
{
    if (rx_ring_len < 2) return 0;                                                             // 连1字节固定头都不够，肯定没完整帧

    uint32_t remaining = 0;                                                                    // 存解码后的剩余长度
    int rl_bytes = 0;                                                                          // 存长度编码占了几个字节
    // 从固定头后开始解码，失败就是数据不完整，等后面数据
    if (mqtt_decode_remaining_length(&rx_ring[1], rx_ring_len - 1, &remaining, &rl_bytes) != 0) // 解码剩余长度失败等后续
        return 0; // 返回0没完整帧

    size_t frame_total = 1 + rl_bytes + remaining;                                             // 整帧长度：1固定头+长度字节+剩余载荷
    if (frame_total > sizeof(rx_ring))                                                         // 单帧比整个缓冲还大，永远装不下
    {
        // 打印超大帧丢弃日志
        printf("[MQTT] drop oversize frame total=%u > ring %u (rejected)\r\n", // 打印超大帧丢弃日志
               (unsigned)frame_total, (unsigned)sizeof(rx_ring)); // printf续行：帧长和缓冲容量
        rx_ring_len = 0;                                                                       // 清空缓冲
        return 0;                                                                              // 返回0
    }
    if (rx_ring_len < frame_total) return 0;                                                   // 缓冲总长度小于帧长，还没收全，等下一段

    // 固定头右移4位，与0x0F取高4位，得到报文类型
    *out_type  = (rx_ring[0] >> 4) & 0x0F; // 高4位是报文类型
    // 与0x0F取低4位，得到固定头标志
    *out_flags = rx_ring[0] & 0x0F; // 低4位是标志
    // 固定头1字节+长度字节，后面就是载荷起始位置
    *out_body  = &rx_ring[1 + rl_bytes]; // 载荷起始指针
    // 载荷长度就是剩余长度
    *out_body_len = remaining; // 载荷长度
    return frame_total;                                                                        // 返回整帧总长度
}

// 函数：rx_ring_consume —— 消费掉已处理的帧
// 功能：一个报文处理完后，把接收环缓冲里这帧的数据搬掉（memmove防源目标重叠），缓冲里可能还留着下一帧甚至夹带的半个帧，搬完继续切
// 参数：frame_total —— 刚处理完那帧的总字节数
// 返回：void，无返回值
static void rx_ring_consume(size_t frame_total) // 消费已处理帧函数
{
    if (frame_total >= rx_ring_len)                               // 这帧长度大于等于缓冲总长度，刚好读完
    {
        rx_ring_len = 0;                                          // 直接清空缓冲长度
    }
    else                                                          // 后面还有数据
    {
        // 源地址是帧后剩余数据，目标是缓冲开头，memmove处理重叠内存安全
        memmove(rx_ring, rx_ring + frame_total, rx_ring_len - frame_total); // memmove搬剩余数据防重叠
        rx_ring_len -= frame_total;                               // 总长度减去已处理帧长度
    }
}

// 函数：mqtt_handle_command —— 处理commands主题的下行命令
// 功能：解析PUBLISH报文：核对主题是commands后，是OTA升级通知就转发升级模块并回受理回执；暂停/静默期直接回忙；普通写寄存器命令手写解析JSON四字段，参数校验后注入Modbus写命令，等执行结果按成功/失败/超时/队列满回cmd_ack回执
// 参数：body —— PUBLISH报文载荷起始指针（含主题）；body_len —— 载荷总长度；publish_flags —— 固定头低4位标志，bit0是RETAIN，保留消息直接拒收防重放
// 返回：void，无返回值，执行结果通过cmd_ack主题回执告知云端
static void mqtt_handle_command(const uint8_t *body, uint32_t body_len, uint8_t publish_flags) // 命令处理函数：3个参数
{
    if (body_len < 4) return;                                    // 至少2字节主题长度+2字节数据，太短直接返回
    // 高字节左移8位或低字节，拼成16位大端主题长度
    uint16_t tlen = ((uint16_t)body[0] << 8) | body[1]; // 大端拼主题长度
    if ((uint32_t)(2 + tlen) > body_len) return;                 // 长度字段比实际载荷还大，坏数据返回

    // 偏移2开始就是主题字符串
    const char *topic = (const char *)&body[2]; // 偏移2是主题字符串
    // 主题结束后就是实际JSON载荷
    const uint8_t *payload = body + 2 + tlen; // 主题后是JSON载荷
    // 总长度减2字节长度字段减主题长度，就是JSON载荷长度
    uint32_t plen = body_len - 2 - tlen; // 载荷长度=总长-2-主题长

    // 固定头最低位就是RETAIN标志
    bool is_retain = (publish_flags & 0x01) != 0; // RETAIN标志位
    if (is_retain)                                               // 是broker存的旧命令，刚上线回放
    {
        printf("[MQTT] reject retained command (RETAIN=1)\r\n"); // 打印拒绝日志
        return;                                                  // 旧命令不执行，直接返回
    }

    // 长度不一样或者内容不一样，就不是commands主题，不处理
    if (tlen != strlen(g_topic_commands) || memcmp(topic, g_topic_commands, tlen) != 0) // 长度和内容都不匹配就不是commands
    {
        return;                                                  // 其他主题不在这里处理
    }

    // 载荷太长就截断到缓冲能放下的最大长度，留1字节放\0
    if (plen >= sizeof(cmd_payload_buf)) plen = sizeof(cmd_payload_buf) - 1; // 载荷截断留结尾
    memcpy(cmd_payload_buf, payload, plen);                      // 拷贝JSON到全局命令缓冲
    cmd_payload_buf[plen] = '\0';                                // 强制字符串结尾，方便字符串函数处理

    printf("[MQTT] recv cmd: %s\r\n", cmd_payload_buf);         // 打印收到的命令调试

#if GATEWAY_ENABLE_M2                                            // 条件编译，M2模块启用才处理OTA
    // 同时包含url和sha256字段，就是OTA升级通知
    if (strstr(cmd_payload_buf, "\"url\"") && strstr(cmd_payload_buf, "\"sha256\"")) // 同时有url和sha256就是OTA通知
    {
        int nr = NetworkUpgrade_Notify(cmd_payload_buf);        // 通知升级模块，只唤醒任务不立刻下载
        char ack[96];                                            // 临时数组存回执JSON
        int alen;                                                // 回执实际长度
        if (nr == 0)                                             // 返回0代表成功受理
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"accepted\",\"type\":\"ota\"}"); // 拼受理成功回执
        }
        else if (nr == -2)                                       // 返回-2代表升级模块忙
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"busy\",\"type\":\"ota\"}"); // 拼忙回执
        }
        else                                                     // 其他错误码
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"type\":\"ota\",\"err\":%d}", nr); // 拼错误回执带错误码
        }
        // 把回执发到命令ACK主题
        MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)ack, (size_t)alen); // 发回执到cmd_ack主题
        return;                                                  // OTA处理完直接返回
    }

    // 配置变更命令：JSON 里包含 "slaves" 字段就是配置变更命令
    // 支持 upsert（新增/更新从站）和 delete（删除从站）两种操作
    // 配置变更由 Config 模块的 Config_HandleMessage 统一处理
    if (strstr(cmd_payload_buf, "\"slaves\""))                  // 包含slaves字段就是配置变更命令
    {
        int cr = Config_HandleMessage(cmd_payload_buf);          // 调Config模块解析并应用配置
        char ack[128];                                            // 临时数组存回执JSON，配置变更回执可能较长
        int alen;                                                // 回执实际长度
        if (cr == 0)                                             // 返回0代表全部成功
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"accepted\",\"type\":\"config\"}"); // 拼成功回执
        }
        else if (cr == -2)                                       // 返回-2代表升级静默期拒绝
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"busy\",\"type\":\"config\",\"err\":\"upgrade\"}"); // 拼忙回执
        }
        else                                                     // 其他错误码（-1解析失败，-3部分失败）
        {
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"type\":\"config\",\"err\":%d}", cr); // 拼错误回执带错误码
        }
        MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)ack, (size_t)alen); // 发回执到cmd_ack主题
        return;                                                  // 配置变更处理完直接返回
    }
#endif                                                           // 条件编译结束

    // 暂停请求了，或者升级静默期不允许改数据
    if (g_pause_requested || !UpgradeQuiesce_AllowsMutation()) // 暂停或静默期判断
    {
        // 固定忙回执字符串
        static const char busy[] = "{\"status\":\"busy\",\"err\":\"upgrade\"}"; // 固定忙回执字符串
        // 发忙回执，sizeof减1去掉字符串结尾\0
        MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)busy, sizeof(busy) - 1U); // 发忙回执sizeof减1
        return;                                                       // 不执行命令直接返回
    }

    // 四个字段默认值，没找到就保持-1，val默认0
    int slave = -1, func = -1, addr = -1, val = 0; // 四字段默认值-1
    // 四个JSON键名字符串，带双引号
    const char *keys[] = { "\"slave\"", "\"func\"", "\"addr\"", "\"val\"" }; // 四个JSON键名带引号
    // 四个输出变量指针，解析结果直接写到对应变量
    int *outs[] = { &slave, &func, &addr, &val }; // 四个输出变量地址
    for (int k = 0; k < 4; k++)                                      // 遍历4个字段
    {
        const char *p = strstr(cmd_payload_buf, keys[k]);            // 在命令JSON里找键名
        if (!p) continue;                                            // 没找到这个字段，跳过下一个
        p += strlen(keys[k]);                                        // 指针跳过键名本身
        // 跳过空格、冒号、制表符，直到碰到值
        while (*p == ' ' || *p == ':' || *p == '\t') p++; // 跳过空格冒号制表符
        // 负号或者数字，就是整数值
        if (*p == '-' || (*p >= '0' && *p <= '9')) // 负号或数字是整数值
        {
            *outs[k] = atoi(p);                                      // 字符串转整数，写到对应变量
        }
    }

    char ack[96];                                                     // 临时回执数组
    int alen;                                                          // 回执长度

    // 从站范围1-247，功能码地址不能是默认-1
    if (slave < 1 || slave > 247 || func < 0 || addr < 0) // 从站1-247功能码地址非-1
    {
        // 拼参数错误回执
        alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":\"bad_cmd\"}"); // 拼参数错误回执
        // 发错误回执
        MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)ack, (size_t)alen); // 发错误回执
        return;                                                       // 参数错不执行
    }

    // 要写的值转16位无符号，Modbus寄存器就是16位
    uint16_t write_val = (uint16_t)val; // 值转16位Modbus寄存器宽度
    if (!mqtt_health_wait(3000U)) return;                             // 登记3秒等待，健康失败直接返回
    // 注入写命令到Modbus模块，参数：从站、功能码、地址、数量1、值、信号量、错误变量、3秒超时
    mb_status_e st = mb_inject_write((uint8_t)slave, // 注入写命令8个参数
                                     (uint8_t)func, // 签名续行：从站功能码
                                     (uint16_t)addr, // 签名续行：地址数量1
                                     1, // 签名续行：值指针
                                     &write_val, // 签名续行：信号量句柄
                                     g_write_sem, // 签名续行：错误码变量
                                     &g_write_err, // 签名续行：错误码变量
                                     3000); // 第8参数：等待Modbus执行结果的超时3秒
    if (!mqtt_health_progress()) return;                              // 注入完喂狗，失败返回

    if (st == MB_STATUS_OK)                                           // 注入成功，排队成功
    {
        if (!mqtt_health_wait(4000U)) return;                         // 登记4秒等待执行结果
        // 等执行完成信号量，4秒超时，毫秒转tick
        osStatus_t status = osSemaphoreAcquire(g_write_sem, pdMS_TO_TICKS(4000U)); // 等4秒执行结果信号量
        if (!mqtt_health_progress()) return;                          // 等完喂狗，失败返回
        if (status == osOK)                                           // 拿到信号量，执行完了
        {
            int errcode = (int)g_write_err;                           // 拿Modbus执行结果错误码
            if (errcode == (int)ERR_OK_QUERY)                         // 执行成功
            {
                // 拼成功回执
                alen = snprintf(ack, sizeof(ack), "{\"status\":\"ok\",\"err\":0}"); // 拼成功回执
            }
            else                                                              // 执行失败
            {
                // 拼失败回执，带Modbus错误码
                alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":%d}", errcode); // 拼失败回执带错误码
            }
        }
        else                                                                  // 信号量超时，没等到执行结果
        {
            // 拼超时回执
            alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":\"timeout\"}"); // 拼超时回执
        }
    }
    else if (st == MB_QUEUE_FULL_ERR)                                         // 注入就失败，命令队列满
    {
        // 拼队列满回执
        alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":\"qfull\"}"); // 拼队列满回执
    }
    else                                                                      // 其他注入错误
    {
        // 拼通用错误回执，带注入错误码
        alen = snprintf(ack, sizeof(ack), "{\"status\":\"err\",\"err\":%d}", (int)st); // 拼注入错误回执
    }

    // 不管成功失败排队结果，都发回执到命令ACK主题
    MqttClient_Publish(g_topic_cmd_ack, (const uint8_t *)ack, (size_t)alen); // 发排队结果回执
}

// 函数：mqtt_handle_ack —— 处理acks主题的业务回执
// 功能：解析回执报文：核对主题是acks后，从载荷里找:seq:标记解析出云端已确认到的最大序列号，构造确认结构投递给存储模块的回收队列让M3释放已确认缓存；投不进队列就等下轮；收到确认同时清补传进度
// 参数：body —— PUBLISH报文载荷起始指针（含主题）；body_len —— 载荷总长度
// 返回：void，无返回值，回收动作通过消息队列异步交给存储模块
static void mqtt_handle_ack(const uint8_t *body, uint32_t body_len) // 业务回执处理函数
{
    if (body_len < 4) return;                                    // 长度不够直接返回
    // 大端2字节主题长度
    uint16_t tlen = ((uint16_t)body[0] << 8) | body[1]; // 大端拼主题长度
    if ((uint32_t)(2 + tlen) > body_len) return;                 // 长度字段非法返回

    // 偏移2开始主题字符串
    const char *topic = (const char *)&body[2]; // 偏移2是主题
    // 主题后是回执JSON载荷
    const uint8_t *payload = body + 2 + tlen; // 主题后是JSON
    // 实际载荷长度
    uint32_t plen = body_len - 2 - tlen; // 载荷长度

    // 长度或者内容不对，就不是acks主题
    if (tlen != strlen(g_topic_acks) || memcmp(topic, g_topic_acks, tlen) != 0) // 不匹配acks主题返回
    {
        return;                                                       // 其他主题不处理
    }

    // 太长截断，留1字节放\0
    if (plen >= sizeof(ack_payload_buf)) plen = sizeof(ack_payload_buf) - 1; // 载荷截断留结尾
    memcpy(ack_payload_buf, payload, plen);                          // 拷贝回执到全局缓冲
    ack_payload_buf[plen] = '\0';                                   // 强制字符串结尾

    uint64_t max_seq = 0;                                           // 存找到的最大序号
    int found = 0;                                                   // 标记有没有找到序号
    const char *p = ack_payload_buf;                                // 遍历指针
    // 找所有":seq:"标记，找不到就退出循环
    while ((p = strstr(p, ":seq:")) != NULL) // 循环找:seq:标记
    {
        p += 5;                                                       // 跳过5个字符:seq:
        uint64_t seq = 0;                                             // 存当前序号
        int got_digit = 0;                                            // 标记有没有读到数字
        // 一直读到不是数字为止
        while (*p >= '0' && *p <= '9') // 连续读数字
        {
            // 十进制累加，字符减'0'转数字
            seq = seq * 10 + (uint64_t)(*p - '0'); // 十进制累加转数
            p++;                                                      // 指针后移
            got_digit = 1;                                            // 标记读到数字
        }
        if (got_digit)                                                // 确实读到了序号
        {
            if (seq > max_seq) max_seq = seq;                        // 保留最大的序号
            found = 1;                                                // 标记找到
        }
    }

    if (found && max_seq > 0)                                        // 找到有效序号
    {
        // C99指定初始化，填存储确认结构体，最大序号之前的都算收到
        storage_ack_t ack = { .ack_upto_seq = max_seq }; // 指定初始化确认结构
        taskENTER_CRITICAL();                                         // 进临界区，关任务调度
        // 没暂停而且允许修改，才允许放队列
        bool allowed = !g_pause_requested && UpgradeQuiesce_AllowsMutation(); // 没暂停且允许修改
        // 允许就非阻塞放队列，不允许直接返回资源错误
        osStatus_t status = allowed ? osMessageQueuePut(storage_m2_ack_queue(), &ack, 0, 0) : osErrorResource; // 允许才非阻塞放队列
        taskEXIT_CRITICAL();                                          // 退出临界区
        if (status != osOK)                                           // 放队列失败
        {
            // 打印延迟处理日志
            printf("[MQTT] biz ack deferred seq=%lu status=%d\r\n", // 打印延迟处理日志
                   (unsigned long)max_seq, (int)status); // printf续行：序号和状态
            return;                                                   // 直接返回，下次再处理
        }
        // 打印收到业务回执日志
        printf("[MQTT] biz ack received, ack_upto_seq=%lu\r\n", (unsigned long)max_seq); // 打印收到回执日志

        // 重置补传状态，刚收到回执不用立刻补传
        g_replay_last_seq = 0; // 清补传序列号
        g_replay_last_tick = 0; // 清补传时间
    }
}

// 函数：mqtt_handle_puback —— 处理QoS1的PUBACK确认
// 功能：broker回了PUBACK说明那个报文ID的包它收到了：遍历待确认槽位找到匹配ID的记录，释放槽位；若是升级启动结果报文额外标记已确认防重复上报；注意PUBACK只清MQTT层待确认，不触发业务缓存回收（那是acks主题的事）
// 参数：packet_id —— broker确认的QoS1报文ID
// 返回：void，无返回值
static void mqtt_handle_puback(uint16_t packet_id) // PUBACK处理函数
{
    if (!mqtt_lock_tx())                                         // 拿发送锁，保护待确认数组
        return;                                                  // 拿不到锁直接返回
    // 遍历所有QoS1待确认槽位
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++) // 遍历待确认槽位
    {
        // 槽位在用，而且报文ID匹配
        if (s_qos1_pending[i].in_use && s_qos1_pending[i].packet_id == packet_id) // 在用且ID匹配
        {
            // 特殊处理启动结果报文，ID匹配
            if (packet_id == g_boot_result_packet_id) // 是启动结果ID判断
            {
                g_boot_result_acked = true;                           // 标记启动结果已确认
                g_boot_result_packet_id = 0;                          // 清空ID，避免重复匹配
            }
            s_qos1_pending[i].in_use = false;                         // 释放槽位，报文ID可以复用
            printf("[MQTT] PUBACK pid=%u\r\n", (unsigned)packet_id); // 打印PUBACK日志
            break;                                                    // 找到匹配就跳出循环
        }
    }
    osMutexRelease(g_tx_mutex);                                       // 释放发送锁
}

// 离线补传驱动，在线就请求存储读未确认记录，每次最多4条，30秒没业务回执就重发，最多60秒间隔
static bool g_fetch_in_flight = false;       // 是否有正在等的取数请求
static uint32_t g_fetch_req_id = 1;          // 下一个请求ID，递增
static uint32_t g_fetch_expected_id;         // 当前等的请求ID，匹配才收

// 大缓冲放CCMRAM，2.2KB，放栈上会爆栈，只有MQTT任务用，static安全
static storage_fetch_resp_t g_fetch_resp __attribute__((section(".bss.ccmram"))); // 取数应答大缓冲CCMRAM

// 函数：mqtt_take_fetch_response —— 取存储模块的补传数据应答
// 功能：非阻塞（0超时）从存储模块的取数应答队列拿一批历史记录到全局大缓冲g_fetch_resp（放CCMRAM省普通RAM）；拿到的应答校验请求ID是否匹配当前在途请求，过期旧应答直接丢弃防串数据
// 参数：无（应答数据存全局g_fetch_resp）
// 返回：bool，取到且ID匹配的有效应答返回真；没取到或旧应答返回假
static bool mqtt_take_fetch_response(void) // 非阻塞拿取数应答函数
{
    // 非阻塞0超时，从队列拿应答，不成功就返回假
    if (osMessageQueueGet(storage_m2_fetch_resp_queue(), &g_fetch_resp, NULL, 0) != osOK) // 0超时非阻塞取队列
        return false; // 没取到返回假
    // 没在等请求，或者应答ID和当前等的ID不匹配，就是旧应答
    if (!g_fetch_in_flight || g_fetch_resp.req_id != g_fetch_expected_id) // 没在途或ID不匹配是旧应答
    {
        // 打印旧应答丢弃日志
        printf("[MQTT] stale fetch response id=%lu expected=%lu\r\n", // 打印旧应答日志
               (unsigned long)g_fetch_resp.req_id, (unsigned long)g_fetch_expected_id); // printf续行：应答ID和期望ID
        return false;                                                // 返回假
    }
    g_fetch_in_flight = false;                                       // 清在途标志，请求结束
    return true;                                                     // 拿到有效应答返回真
}

// 函数：mqtt_pause_checkpoint —— 暂停状态检查点
// 功能：主循环业务间隙调用：若升级模块请求了暂停，等在途的补传拉取收尾（无在途请求）且健康正常时才正式置“暂停已确认”，之后升级模块才敢动flash，保证暂停瞬间没有半途的收发
// 参数：无
// 返回：void，无返回值，结果落在全局g_pause_acknowledged标志
static void mqtt_pause_checkpoint(void) // 暂停检查点函数
{
    if (g_pause_requested)                                       // 已经请求暂停
        (void)mqtt_take_fetch_response();                        // 先把在途取数应答收掉，强转void忽略结果
    taskENTER_CRITICAL();                                        // 进临界区保护标志
    // 三个条件同时满足：暂停请求了、没在途请求、健康正常，才标记暂停确认
    g_pause_acknowledged = g_pause_requested && !g_fetch_in_flight && mqtt_health_progress(); // 请求+无在途+健康才确认
    taskEXIT_CRITICAL();                                         // 退出临界区
}

// 函数：mqtt_allows_publish —— 主题发送准入检查
// 功能：暂停/升级静默期不是所有主题都禁发：status在线状态、cmd_ack命令回执、ota_status升级进度这三类暂停时也必须能发（否则升级状态上不去云）；业务主题（遥测/事件）则要求非暂停且升级模块允许改动
// 参数：topic —— 待检查的主题字符串
// 返回：bool，允许发送返回真；禁止发送返回假
static bool mqtt_allows_publish(const char *topic) // 主题发送准入函数
{
    // 三个特殊主题任意一个匹配，或者非暂停且允许修改，就可以发
    return strcmp(topic, g_topic_status) == 0 || strcmp(topic, g_topic_cmd_ack) == 0 || // 特殊主题或非暂停可发
           strcmp(topic, g_topic_ota_status) == 0 || // printf续行：准入判断
           (!g_pause_requested && UpgradeQuiesce_AllowsMutation()); // 非暂停且允许修改
}

// 函数：MqttClient_SetPaused —— 设置暂停/恢复（对外接口）
// 功能：给升级模块调的公开接口：进临界区原子地设置暂停请求标志，同时清掉旧的“暂停已确认”，让MQTT任务重新走一遍确认流程，防止沿用上一个暂停周期的旧状态
// 参数：paused —— 真=请求暂停（等MQTT确认后停业务收发）；假=恢复运行
// 返回：void，无返回值，真暂停与否可用MqttClient_IsPaused查询
void MqttClient_SetPaused(bool paused) // 设置暂停接口函数
{
    taskENTER_CRITICAL();                                        // 进临界区保护全局标志
    g_pause_requested = paused;                                  // 设置暂停/恢复标志
    g_pause_acknowledged = false;                                // 清旧确认，必须重新确认新状态
    taskEXIT_CRITICAL();                                         // 退出临界区
}

// 函数：MqttClient_IsPaused —— 查询是否真暂停（对外接口）
// 功能：给升级模块调的公开接口：“真暂停”要同时满足三条件：请求了暂停、MQTT任务已确认（收尾完成）、当前能拿到发送锁（没有任务卡在发送中间态），缺一都不算，防止升级时还有报文在路上
// 参数：无
// 返回：bool，真暂停返回真；没请求暂停或还没确认完返回假
bool MqttClient_IsPaused(void) // 查询真暂停函数
{
    taskENTER_CRITICAL();                                        // 进临界区读标志
    // 两个标志同时真，才是暂停流程走完了
    bool paused = g_pause_requested && g_pause_acknowledged; // 请求且确认才是暂停
    taskEXIT_CRITICAL();                                         // 退出临界区
    // 没暂停、锁没初始化、非阻塞拿不到锁（还在发数据），都不算真暂停
    if (!paused || g_tx_mutex == NULL || osMutexAcquire(g_tx_mutex, 0U) != osOK) // 没暂停或拿不到锁不算
        return false; // 返回假
    osMutexRelease(g_tx_mutex);                                  // 拿到锁马上释放，只是确认没在发
    return true;                                                 // 真暂停返回真
}

// 函数：mqtt_replay_pump —— 离线补传泵
// 功能：主循环每3秒调一次，驱动整个补传流程：先收上一批拉取的应答，把历史记录逐条按类型（事件/遥测）QoS1发出并记最大序列号；同批30秒没等到业务回执就按原ID重发；都确认完清进度，没在途请求就发新一批拉取（每批最多4条省内存）
// 参数：无（数据从存储模块队列来，发往events/telemetry主题）
// 返回：void，无返回值，进度记在全局g_replay_last_seq/g_replay_last_tick
static void mqtt_replay_pump(void) // 补传泵驱动函数
{
    // 第一步：非阻塞检查存储模块的取数应答
    if (mqtt_take_fetch_response()) // 先收存储取数应答
    {
        // 暂停了或者不允许修改，直接返回不发
        if (g_pause_requested || !UpgradeQuiesce_AllowsMutation()) return; // 暂停或静默直接返回

        if (g_fetch_resp.count > 0)                                   // 这批拿到了历史记录
        {
            uint64_t max_seq = 0;                                      // 这批最大序号
            // 遍历这批所有记录，事件发events，遥测发telemetry，都用QoS1
            for (int i = 0; i < g_fetch_resp.count; i++) // 遍历这批记录
            {
                // 状态变了就跳出，不继续发
                if (g_pause_requested || !UpgradeQuiesce_AllowsMutation()) break; // 状态变了跳出
                record_out_t *r = &g_fetch_resp.records[i];              // 当前记录指针
                // 事件类型用events主题，其他遥测类型用telemetry
                const char *topic = (r->kind == RECORD_KIND_EVENT) ? g_topic_events : g_topic_telemetry; // 事件走events其他走telemetry
                // QoS1发送，返回0成功
                if (MqttClient_PublishQos1(topic, r->payload, r->payload_len) == 0) // QoS1发返回0成功
                {
                    // 更新最大序号
                    if (r->seq > max_seq) max_seq = r->seq; // 更新最大序号
                }
                else                                                      // 发送失败
                {
                    break;                                                // 本轮停掉，下轮再试
                }
            }

            if (max_seq > 0)                                              // 确实发出去了
            {
                // 还是同一批没推进，而且之前发过
                if (max_seq == g_replay_last_seq && g_replay_last_tick > 0) // 同批没推进且发过
                {
                    // 算距离上次发过了多久
                    uint32_t elapsed = osKernelGetTickCount() - g_replay_last_tick; // 算距上次多久
                    if (elapsed < MQTT_REPLAY_RESEND_MS)                  // 没到30秒
                    {
                        return;                                           // 不重发，等够时间
                    }
                }
                // 记录当前最大序号和发送时间
                g_replay_last_seq = max_seq; // 记当前最大序号
                g_replay_last_tick = osKernelGetTickCount(); // 记当前时间
                // 打印补传日志，说明在等业务回执
                printf("[MQTT] replay %u records, max_seq=%lu (await biz ack)\r\n", // 打印补传日志
                       (unsigned)g_fetch_resp.count, (unsigned long)max_seq); // printf续行：条数和序号
            }
        }
        else                                                              // 这批count=0，没历史数据了
        {
            // 重置补传追踪
            g_replay_last_seq = 0; // 清补传序号
            g_replay_last_tick = 0; // 清补传时间
        }
    }

    // 第二步：没有在途请求，发新的拉取请求，每次最多4条
    if (!g_fetch_in_flight) // 无在途发新拉取
    {
        // 指定初始化拉取请求，请求ID和每批最大条数
        storage_fetch_req_t req = { // 指定初始化拉取请求
            .req_id = g_fetch_req_id, // 请求ID
            .max_count = STORAGE_M2_FETCH_BATCH_MAX, // 每批最多4条
        };
        taskENTER_CRITICAL();                                        // 进临界区
        // 没暂停而且允许修改才发
        bool allowed = !g_pause_requested && UpgradeQuiesce_AllowsMutation(); // 没暂停且允许修改
        // 允许而且非阻塞放队列成功
        if (allowed && osMessageQueuePut(storage_m2_fetch_req_queue(), &req, 0, 0) == osOK) // 允许且放队列成功
        {
            g_fetch_in_flight = true;                                    // 标记在途
            g_fetch_expected_id = req.req_id;                            // 记住要等的ID
            // 先自增，溢出回1，ID不能为0
            if (++g_fetch_req_id == 0U) g_fetch_req_id = 1U; // 自增溢出回1
        }
        taskEXIT_CRITICAL();                                             // 退出临界区
    }
}

// 函数：mqtt_qos1_check_resend —— QoS1超时重发检查
// 功能：主循环反复调用：遍历QoS1待确认槽位，在用、主题允许发、距发送超30秒还没等到PUBACK的报文，用原来的主题/载荷/报文ID加DUP=1重发一次（DUP=1告诉broker这是重发的旧包不是新包）；发送失败则释放槽位
// 参数：无（待确认队列在全局s_qos1_pending数组）
// 返回：void，无返回值
static void mqtt_qos1_check_resend(void) // QoS1超时重发检查函数
{
    // 遍历所有待确认槽位
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++) // 遍历待确认槽位
    {
        if (!mqtt_lock_tx())                                         // 拿发送锁
            return;                                                  // 拿不到锁直接返回
        uint32_t now = osKernelGetTickCount();                       // 当前系统tick
        // 三个条件任意一个：槽位没用、当前不能发这个主题、没到30秒
        if (!s_qos1_pending[i].in_use || !mqtt_allows_publish(s_qos1_pending[i].topic) || // 没用/不能发/没到30秒
            (now - s_qos1_pending[i].send_tick) < MQTT_REPLAY_RESEND_MS) // printf续行：三条件判断
        {
            osMutexRelease(g_tx_mutex);                                  // 释放锁
            continue;                                                    // 跳过这个槽位下一个
        }

        // 超时重发，保持原主题、原数据、原ID、QoS1，DUP=true标记重发
        size_t pkt_len = mqtt_build_publish(s_qos1_pending[i].topic, // 重发：原主题原数据原ID
                                            s_qos1_pending[i].payload, // printf续行：载荷
                                            s_qos1_pending[i].payload_len, // printf续行：长度
                                            s_qos1_pending[i].retain, 1, // printf续行：retain QoS1
                                            s_qos1_pending[i].packet_id, true); // printf续行：ID DUP=true
        err_t err = mqtt_write(tx_buf, pkt_len);                       // 发重发包
        if (err == ERR_OK)                                              // 发送成功
        {
            s_qos1_pending[i].send_tick = now;                          // 更新发送时间，重新等30秒
            // 打印重发日志
            printf("[MQTT] QoS1 resend pid=%u (30s timeout)\r\n", // 打印重发日志
                   (unsigned)s_qos1_pending[i].packet_id); // printf续行：报文ID
        }
        osMutexRelease(g_tx_mutex);                                      // 释放锁
        if (err != ERR_OK) return;                                       // 发送失败直接返回，下轮再试
    }
}

// 函数：MqttClient_Init —— 模块初始化（对外接口）
// 功能：开机装配时调一次：防重入（对象已建直接返回成功）；创建发送互斥锁和写命令结果信号量，信号量创建失败会删刚建的锁防泄漏；用系统tick做随机数种子给重连抖动用；最后加载配置、算百度云认证、拼7个主题
// 参数：无
// 返回：AppModuleInitResult，APP_MODULE_INIT_OK代表成功；APP_MODULE_INIT_FAILED代表内核对象创建失败，上层应中止启动
AppModuleInitResult MqttClient_Init(void) // 模块初始化函数
{
    // 两个内核对象都已经创建，就是初始化过了
    if (g_tx_mutex != NULL && g_write_sem != NULL) // 两对象都在就是初始化过
        return APP_MODULE_INIT_OK; // 返回成功

    if (g_tx_mutex == NULL)                                      // 还没创建互斥锁
    {
        g_tx_mutex = osMutexNew(&g_tx_mutex_attr);                    // 创建静态分配的互斥锁
        if (g_tx_mutex == NULL)                                       // 创建失败
            return APP_MODULE_INIT_FAILED; // 返回初始化失败，上层会重试
    }

    // 创建1计数信号量，初始值0，写命令等结果用
    g_write_sem = osSemaphoreNew(1, 0, NULL); // 建1计数信号量初值0
    if (g_write_sem == NULL)                                      // 信号量创建失败
    {
        // 删掉刚创建的互斥锁，避免资源泄漏
        if (osMutexDelete(g_tx_mutex) == osOK) // 删锁防资源泄漏
            g_tx_mutex = NULL; // 锁句柄置空
        return APP_MODULE_INIT_FAILED; // 信号量没了也算初始化失败返回
    }

    // 用当前tick做随机数种子，重连抖动随机数用
    srand(osKernelGetTickCount()); // tick做随机数种子

    mqtt_config_load();                                               // 加载broker地址端口等配置

#if MQTT_USE_BAIDU                                                   // 百度云模式
    // 关键顺序：先算认证，把client_id设为设备key，再拼主题
    baidu_build_auth(); // 先算认证再拼主题
#endif                                                               // 条件编译结束

    mqtt_topics_build();                                              // 拼7个完整主题

    // 初始化所有运行状态，避免开机随机值
    g_conn = NULL;                                                   // netconn指针清空
    g_connected = false;                                             // 连接标志假
    rx_ring_len = 0;                                                 // 接收环形缓冲清空
    g_fetch_in_flight = false;                                       // 取数在途标志假
    g_backoff_attempt = 0;                                           // 重连次数清零
    g_replay_last_seq = 0;                                           // 补传序号清零
    g_replay_last_tick = 0;                                          // 补传时间清零
    return APP_MODULE_INIT_OK;                                       // 初始化成功
}

// 函数：MqttClient_IsConnected —— 查询连接状态（对外接口）
// 功能：给别的模块（遥测任务等）查MQTT当前是否在线：TCP连接建立且CONNACK/SUBACK都通过、主循环置位g_connected后才算真在线；离线期间遥测走M3缓存不直发
// 参数：无
// 返回：bool，业务在线返回真；未连接或断线重连中返回假
bool MqttClient_IsConnected(void) // 查询连接状态接口
{
    return g_connected;                                          // 返回全局连接标志，真连上假断开
}

// 函数：MqttClient_Publish —— QoS0发布（对外接口）
// 功能：给遥测/事件等调的公开发布接口，QoS0发完就完不等确认：校验初始化、载荷不超1024、总长不超发送缓冲，拿锁后检查在线和主题准入，组包TCP发出，全程持锁防并发
// 参数：topic —— 主题字符串；payload —— 载荷指针（JSON字符串）；plen —— 载荷字节数
// 返回：int，0代表发送成功；-1没连接/没初始化，-2发送失败，-3超长被拒，-5主题当前禁止发送
int MqttClient_Publish(const char *topic, const uint8_t *payload, size_t plen) // 对外QoS0发布接口
{
    if (g_tx_mutex == NULL)                                      // 模块还没初始化，锁不存在
    {
        return -1;                                                   // 返回未初始化错误
    }

    if (plen > MQTT_MAX_PAYLOAD)                                  // 载荷超过最大1024
    {
        return -3;                                                   // 超长拒绝，不截断
    }
    size_t tlen = strlen(topic);                                   // 算主题长度
    // 加8字节头部余量，总长度超过发送缓冲，装不下
    if (tlen + plen + 8 > sizeof(tx_buf)) // 总长超发送缓冲
    {
        return -3;                                                   // 拒绝发送
    }

    if (!mqtt_lock_tx())                                           // 拿发送锁
        return -1;                                                  // 拿锁失败返回
    // 没连接或者连接指针空
    if (!g_connected || g_conn == NULL) // 没连接或句柄空
    {
        osMutexRelease(g_tx_mutex);                                  // 释放锁
        return -1;                                                   // 没连不能发
    }

    if (!mqtt_allows_publish(topic))                               // 当前状态不允许发这个主题
    {
        osMutexRelease(g_tx_mutex);                                  // 释放锁
        return -5;                                                   // 状态不允许错误
    }

    // QoS0，不retain，无报文ID，非重发，组包
    size_t pkt_len = mqtt_build_publish(topic, payload, plen, false, 0, 0, false); // QoS0组包不retain
    err_t err = mqtt_write(tx_buf, pkt_len);                      // TCP发送

    osMutexRelease(g_tx_mutex);                                   // 释放锁
    // 成功返回0，发送失败返回-2
    return (err == ERR_OK) ? 0 : -2; // 成功0失败-2
}

// 内部QoS1发送，存待确认队列，等PUBACK，启动结果特殊标记，返回0已发送1已确认
// 函数：mqtt_publish_qos1 —— QoS1发布（内部实现）
// 功能：QoS1发布的内部实现：比QoS0多一套待确认机制——找空闲槽位存主题/载荷/报文ID/发送时间，用do-while生成不重复的报文ID，组包发送后等PUBACK（30秒没到由mqtt_qos1_check_resend重发）；boot_result参数走特殊通道防升级结果丢
// 参数：topic —— 主题字符串；payload —— 载荷指针；plen —— 载荷长度；boot_result —— 真表示这是升级启动结果报文要等确认并防重复上报，假是普通业务发布
// 返回：int，0代表已发出等PUBACK确认；1代表启动结果已被确认过（直接成功）；-1没连接，-2发送失败，-3超长，-4槽满，-5禁止发送
static int mqtt_publish_qos1(const char *topic, const uint8_t *payload, size_t plen, bool boot_result) // 内部QoS1发送函数
{
    if (g_tx_mutex == NULL)                                      // 互斥锁句柄是空的，说明开机没成功创建这把锁（模块没初始化），不能往下发
    {
        return -1; // 锁都没建起来，直接返回错误码-1，不做发送
    }

    if (plen > MQTT_MAX_PAYLOAD)                                  // 超过1024总上限
    {
        return -3; // 返回超长
    }
    size_t tlen = strlen(topic);                                   // 主题长度
    // 加10字节余量，装不下缓冲
    if (tlen + plen + 10 > sizeof(tx_buf)) // 加10余量超缓冲
    {
        return -3; // 返回拒绝
    }
    if (plen > MQTT_QOS1_PAYLOAD_MAX)                              // 待确认槽只能存512，更严格
    {
        return -3; // 返回超长
    }

    if (!mqtt_lock_tx())                                           // 拿锁
        return -1; // 拿锁失败返回
    // 启动结果，而且已经确认或者在途，不重复发
    if (boot_result && (g_boot_result_acked || (g_boot_result_packet_id != 0U))) // 启动结果已确认或在途
    {
        // 确认返回1，在途返回0
        int result = g_boot_result_acked ? 1 : 0; // 确认1在途0
        osMutexRelease(g_tx_mutex); // 释放锁
        return result; // 返回状态
    }
    // 没连接
    if (!g_connected || g_conn == NULL) // 没连接
    {
        osMutexRelease(g_tx_mutex); // 释放锁
        return -1; // 返回没连接
    }

    if (!mqtt_allows_publish(topic))                               // 主题准入失败
    {
        osMutexRelease(g_tx_mutex); // 释放锁
        return -5; // 返回状态错误
    }
    int slot = -1;                                                  // 空闲槽位，默认-1没找到
    // 遍历待确认槽
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++) // 遍历待确认槽
    {
        // 找到没用的槽，记下跳出
        if (!s_qos1_pending[i].in_use) { slot = i; break; } // 找到空闲槽跳出
    }
    if (slot < 0)                                                   // 没找到空闲槽
    {
        osMutexRelease(g_tx_mutex); // 释放锁
        return -4;                                                   // 槽满错误
    }

    uint16_t pid;                                                   // 报文ID
    bool id_in_use;                                                 // 标记ID是不是已经在用
    do // do-while先取一次
    {
        // 先取ID再自增
        pid = g_next_packet_id++; // 取ID再自增
        if (g_next_packet_id == 0) g_next_packet_id = 1;           // 溢出回1
        id_in_use = false;                                          // 默认没用
        // 遍历所有槽，检查ID是不是重复
        for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++) // 遍历查ID重复
        {
            // 槽在用而且ID一样，ID重复
            if (s_qos1_pending[i].in_use && s_qos1_pending[i].packet_id == pid) // 在用且同ID
                id_in_use = true; // 标记重复
        }
    } while (id_in_use);                                              // ID重复就再换一个

    // 标记槽在用
    s_qos1_pending[slot].in_use      = true; // 标记槽在用
    // 存报文ID
    s_qos1_pending[slot].packet_id   = pid; // 存报文ID
    // 存发送时间，用来算超时
    s_qos1_pending[slot].send_tick   = osKernelGetTickCount(); // 存发送时间
    // 不retain
    s_qos1_pending[slot].retain      = false; // 不retain
    // 存载荷长度
    s_qos1_pending[slot].payload_len = plen; // 存载荷长度
    // 拷贝主题，留1字节\0
    strncpy(s_qos1_pending[slot].topic, topic, sizeof(s_qos1_pending[slot].topic) - 1); // 拷主题留结尾
    s_qos1_pending[slot].topic[sizeof(s_qos1_pending[slot].topic) - 1] = '\0'; // 强制字符串结尾
    // 拷贝载荷，调用方数据之后可以释放
    memcpy(s_qos1_pending[slot].payload, payload, plen); // 拷载荷

    // 组QoS1报文，不retain，非重发
    size_t pkt_len = mqtt_build_publish(topic, payload, plen, false, 1, pid, false); // 组QoS1报文
    err_t err = mqtt_write(tx_buf, pkt_len);                        // 发送

    if (err != ERR_OK)                                              // 发送失败
    {
        s_qos1_pending[slot].in_use = false;                        // 释放槽位
        osMutexRelease(g_tx_mutex); // 释放锁
        return -2; // 返回发送失败
    }
    if (boot_result)                                                // 是启动结果
        g_boot_result_packet_id = pid;                              // 记下ID，等PUBACK
    osMutexRelease(g_tx_mutex);                                     // 释放锁
    return 0;                                                       // 已发送，等确认
}

// 函数：MqttClient_PublishQos1 —— QoS1发布（对外接口）
// 功能：给业务模块调的公开QoS1发布接口：内部转调mqtt_publish_qos1并标记非启动结果，发布后进入待确认队列，broker不回PUBACK就30秒自动重发
// 参数：topic —— 主题字符串；payload —— 载荷指针；plen —— 载荷字节数
// 返回：int，0代表已发送等确认；负数代表各类失败（错误码含义见mqtt_publish_qos1）
int MqttClient_PublishQos1(const char *topic, const uint8_t *payload, size_t plen) // 对外QoS1发布接口
{
    // 调内部实现，不是启动结果
    return mqtt_publish_qos1(topic, payload, plen, false); // 调内部实现非启动结果
}

// 函数：MqttClient_PublishUpgradeResult —— 上报升级启动结果（对外接口）
// 功能：升级模块专用：把升级是否启动成功的结果发到ota_status主题并标记boot_result，确认前断线会保持原报文ID自动重发，重复调用只查状态不重发（返回1代表云端已确认收到），保证升级结果不丢
// 参数：payload —— 结果JSON载荷指针；plen —— 载荷长度
// 返回：int，0代表已发送等确认；1代表已确认过；-3代表参数错（空指针/长度0/主题没拼好）
int MqttClient_PublishUpgradeResult(const uint8_t *payload, size_t plen) // 升级启动结果上报接口
{
    // 空指针、长度0、OTA主题没初始化，参数错误
    if ((payload == NULL) || (plen == 0U) || (g_topic_ota_status[0] == '\0')) // 参数校验三条件
        return -3; // 返回参数错误
    // 发OTA状态主题，标记是启动结果
    return mqtt_publish_qos1(g_topic_ota_status, payload, plen, true); // 发OTA主题标记启动结果
}

// 函数：MqttClient_ReportEvent —— 事件上报（对外接口）
// 功能：把事件包装成{"type":"...","detail":"..."}格式的JSON，QoS0发到events主题，开机事件、错误事件都走这个；空指针参数按空串处理防崩溃
// 参数：event_type —— 事件类型短字符串（如boot）；detail —— 事件详情描述，可以为NULL
// 返回：int，0代表发送成功；负数错误码含义同MqttClient_Publish
int MqttClient_ReportEvent(const char *event_type, const char *detail) // 事件上报接口
{
    char json[128];                                              // 临时数组存事件JSON
    // 拼JSON，空指针就传空字符串，避免野指针
    int len = snprintf(json, sizeof(json), "{\"type\":\"%s\",\"detail\":\"%s\"}", // 拼JSON空指针传空串
                       event_type ? event_type : "", // printf续行：类型
                       detail ? detail : ""); // printf续行：详情
    if (len <= 0) return -1;                                     // 拼接失败返回错误
    // QoS0发事件主题
    return MqttClient_Publish(g_topic_events, (const uint8_t *)json, (size_t)len); // QoS0发events主题
}

// 函数：wait_network_ready —— 等待网络就绪
// 功能：连接broker前先等底层网络真正可用：物理链路通（等网线插好）、默认网卡netif已up、拿到非全0的IP（DHCP成功），三样齐了才算就绪；轮询间隔补齐到1秒并持续喂狗，超时返回假
// 参数：timeout_ms —— 最长等待毫秒数，主任务传60秒
// 返回：bool，网络就绪返回真；超时或健康上报失败返回假
static bool wait_network_ready(uint32_t timeout_ms) // 等网络就绪函数
{
    uint32_t started_ms = HAL_GetTick();                          // 记录开始时间
    // 没超时就循环
    while ((uint32_t)(HAL_GetTick() - started_ms) < timeout_ms) // 没超时循环
    {
        uint32_t poll_ms = HAL_GetTick();                          // 本轮轮询开始时间
        if (!mqtt_health_wait(1000U))                              // 登记1秒等待
            return false; // 健康失败返回假
        // 等物理链路，1秒超时
        int32_t link = m2_ethernet_link_wait(1000U); // 等1秒物理链路
        if (!mqtt_health_progress())                               // 喂狗
            return false; // 健康失败返回假
        if (link == ETH_OK)                                        // 物理链路通
        {
            // 默认netif存在、netif启用、IP不是0.0.0.0，三个条件同时满足
            if (netif_default != NULL && // 默认netif存在
                netif_is_up(netif_default) && // netif已启用
                !ip4_addr_isany(netif_ip4_addr(netif_default))) // IP非全0
            {
                return true;                                              // 网络就绪返回真
            }
        }
        // 算本轮已经过了多久
        uint32_t elapsed = (uint32_t)(HAL_GetTick() - poll_ms); // 算本轮耗时
        // 没到1秒，补齐剩下的延时，保持1秒轮询
        if (elapsed < 1000U && !mqtt_delay(1000U - elapsed)) // 补齐到1秒轮询
            return false; // 延时失败返回假
    }
    return false;                                                     // 超时还没就绪返回假
}

// 函数：mqtt_connection_delete —— 删除旧连接
// 功能：断线后清理：先清QoS1待确认槽位（防止重连后重发已作废的包），再删netconn释放TCP资源，删完指针置空防野指针，最后喂狗；失败时不覆盖旧指针，拿不到锁时20秒内只删一次，防止删连接把系统卡死
// 参数：无（操作全局g_conn和相关状态）
// 返回：bool，删干净且健康正常返回真；拿不到锁或删除失败返回假，调用方下轮再试
static bool mqtt_connection_delete(void) // 删旧连接函数
{
    g_connected = false;                                          // 先关连接标志，新发送进不来
    if (!mqtt_lock_tx())                                          // 拿发送锁
        return false; // 拿不到返回假

    if (!mqtt_health_wait(20000U))                                // 登记20秒删除等待
    {
        osMutexRelease(g_tx_mutex); // 释放锁
        return false; // 返回假
    }
    // 旧连接的QoS1身份不能带到新连接，全清，业务数据保留后面重发
    for (int i = 0; i < MQTT_QOS1_PENDING_MAX; i++) // 清QoS1在用标志
        s_qos1_pending[i].in_use = false; // 循环清槽位
    g_boot_result_packet_id = 0;                                  // 清启动结果ID
    err_t err = netconn_delete(g_conn);                           // 删netconn，释放LwIP资源
    if (err == ERR_OK)                                            // 删除成功
        g_conn = NULL;                                            // 指针清空，避免野指针
    bool healthy = mqtt_health_progress();                        // 喂狗
    osMutexRelease(g_tx_mutex);                                   // 释放锁
    if (err != ERR_OK)                                            // 删除失败
        printf("[MQTT] connection delete failed err=%d\r\n", (int)err); // 打印删除失败
    // 删除成功而且健康才返回真
    return err == ERR_OK && healthy; // 成功且健康才真
}

// 函数：MqttClient_Task —— MQTT主任务
// 功能：FreeRTOS任务入口，跑整个MQTT状态机大循环：等网络就绪→DNS解析→TCP连接→发CONNECT等CONNACK→订阅等SUBACK→进主循环（心跳保活/补传泵/QoS1重发/收包分发命令和回执）→断线按1/2/4/8/16/32/60秒+抖动退避重连；健康失败则清状态退出任务
// 参数：argument —— FreeRTOS任务参数，本任务没用到，传NULL
// 返回：void，无返回值，健康失败时调用osThreadExit退出任务线程
void MqttClient_Task(void *argument) // MQTT主任务函数
{
    (void)argument;                                              // 没用到任务参数，消除编译警告
    g_mqtt_thread = osThreadGetId();                             // 存自己线程ID，健康检查判断用
    // 登记健康监控：2秒内要喂一次，30秒超时判定卡死
    if (!Monitor_RegisterHealth(APP_MODULE_MQTT, 2000U, 30000U)) // 登记2秒喂狗30秒超时
        goto health_fail;                                        // 登记失败跳健康失败退出
    // 打印任务启动日志，显示broker、客户端ID、有没有认证
    printf("[MQTT] task started, broker=%s:%u cid=%s auth=%s\r\n", // 打印任务启动日志
           g_cfg.broker_host, g_cfg.broker_port, g_cfg.client_id, // printf续行：broker端口
           g_cfg.username[0] ? "yes" : "no"); // printf续行：ID和认证

    while (1)                                                     // 主无限循环，任务永远在这里跑
    {
        // 有旧连接而且删除失败
        if (g_conn != NULL && !mqtt_connection_delete()) // 有旧连接删失败判断
        {
            // 退避延时，延时失败跳健康失败
            if (!mqtt_delay(mqtt_backoff_delay(g_backoff_attempt++))) goto health_fail; // 退避延时失败跳健康
            continue;                                                    // 下一轮再删
        }

        // 第一步：等网络就绪，最多60秒
        if (!wait_network_ready(60000)) // 等60秒网络就绪
        {
            printf("[MQTT] network not ready, retry\r\n"); // 打印网络没就绪
            // 等5秒再试，失败跳健康失败
            if (!mqtt_delay(5000)) goto health_fail; // 等5秒重试
            continue; // 继续下一轮
        }
        // 打印当前拿到的IP地址
        printf("[MQTT] network ready, IP=%s\r\n", // 打印当前IP
               ipaddr_ntoa(netif_ip4_addr(netif_default))); // printf续行：IP字符串

        // 第二步：DNS解析域名，最多等20秒
        printf("[MQTT] resolving %s ...\r\n", g_cfg.broker_host); // 打印解析日志
        if (!mqtt_health_wait(20000U)) goto health_fail; // 登记20秒DNS等待
        // 域名转IP，结果存broker_ip
        err_t dnserr = netconn_gethostbyname(g_cfg.broker_host, &g_cfg.broker_ip); // 域名转IP
        if (!mqtt_health_progress()) goto health_fail; // 喂狗
        if (dnserr != ERR_OK)                                             // DNS失败
        {                                                                 
            // 打印DNS失败日志
            printf("[MQTT] DNS resolve '%s' failed err=%d\r\n", g_cfg.broker_host, (int)dnserr); // 打印DNS失败
            // 退避后下一轮重试
            if (!mqtt_delay(mqtt_backoff_delay(g_backoff_attempt++))) goto health_fail; // 退避后重试
            continue; // 继续下一轮
        }                                                                 
        g_cfg.broker_ip_valid = true;                                     // 标记IP有效
        // 打印域名和解析后的IP，开始连接
        printf("[MQTT] DNS %s -> %s, connecting ...\r\n", // 打印解析结果
               g_cfg.broker_host, ipaddr_ntoa(&g_cfg.broker_ip)); // printf续行：域名和IP

#if MQTT_USE_BAIDU                                                       // 百度云模式
        // 每次连接重新算签名，虽然时间戳固定0，保持流程一致
        baidu_build_auth(); // 每次连接重算签名
#endif                                                                 // 百度云条件编译结束

        if (!mqtt_health_wait(1000U)) goto health_fail;                // 登记1秒创建等待
        // 创建TCP类型netconn，绑定事件回调，非阻塞连接用
        g_conn = netconn_new_with_callback(NETCONN_TCP, mqtt_netconn_event); // 建TCP netconn带回调
        if (!mqtt_health_progress()) goto health_fail;                 // 喂狗
        if (g_conn == NULL)                                            // 内存不足创建失败
        {                                                                 
            printf("[MQTT] netconn_new failed\r\n"); // 打印创建失败
            // 退避后重试
            if (!mqtt_delay(mqtt_backoff_delay(g_backoff_attempt++))) goto health_fail; // 退避后重试
            continue; // 继续下一轮
        }                                                                 

        // 关键：接收超时1秒，定期醒，才能发心跳跑补传，不然永久阻塞会心跳超时
        netconn_set_recvtimeout(g_conn, 1000); // 收超时1秒定期醒
        // 发送超时10秒
        netconn_set_sendtimeout(g_conn, MQTT_IO_TIMEOUT_MS); // 发超时10秒

        // 发起TCP连接
        err_t err = mqtt_connect(); // 发起TCP连接
        if (err != ERR_OK)                                             // TCP连接失败
        {                                                                 
            printf("[MQTT] TCP connect failed err=%d\r\n", (int)err); // 打印连接失败
            goto conn_fail;                                            // 跳连接失败标签，删连接
        }                                                                 

        // 第三步：发CONNECT，先清空接收环形缓冲，避免旧数据干扰
        rx_ring_len = 0; // 清接收缓冲
        size_t pkt_len = mqtt_build_connect();                         // 组CONNECT报文
        printf("[MQTT] CONNECT %u bytes, sending...\r\n", (unsigned)pkt_len); // 打印发送日志
        err = mqtt_write(tx_buf, pkt_len);                             // 发送CONNECT
        if (err != ERR_OK)                                             // 发送失败
        {                                                                 
            printf("[MQTT] CONNECT write failed err=%d\r\n", (int)err); // 打印发送失败
            goto conn_fail; // 跳连接失败
        }                                                                 

        // 第四步：等CONNACK，最多10秒，单独代码块限制变量作用域
        {
            bool got_connack = false;                                  // 标记有没有收到CONNACK
            uint32_t t0 = osKernelGetTickCount();                     // 开始等待时间
            // 没超时就循环
            while ((osKernelGetTickCount() - t0) < MQTT_CONNACK_TIMEOUT_MS) // 10秒超时循环
            {
                struct netbuf *nb;                                          // 存收到的网络缓冲
                err = mqtt_receive(&nb);                                    // 1秒超时等数据
                if (err == ERR_OK && nb != NULL)                            // 成功收到数据
                {                                                                 
                    void *dp; uint16_t dl;                                   // 数据指针和长度
                    netbuf_data(nb, &dp, &dl);                               // 从netbuf拿数据指针
                    // 追加到环形拼包缓冲，处理TCP分包
                    rx_ring_append((const uint8_t *)dp, dl); // 追加到拼包缓冲
                    netbuf_delete(nb);                                       // 删netbuf释放内存

                    uint8_t mtype, mflags;                                   // 报文类型和标志
                    const uint8_t *mbody;                                    // 报文载荷指针
                    uint32_t mbody_len;                                      // 载荷长度
                    // 尝试取完整帧，不完整返回0
                    size_t flen = rx_ring_take_frame(&mtype, &mflags, &mbody, &mbody_len); // 取完整帧
                    if (flen > 0)                                            // 拿到完整帧
                    {                                                                 
                        if (mtype == 2)                                       // 类型2就是CONNACK
                        {                                                                 
                            // 载荷至少2字节，第二字节是返回码，不够就0xFF坏数据
                            uint8_t rc = (mbody_len >= 2) ? mbody[1] : 0xFF; // 第二字节返回码
                            // 打印返回码，注释每个值含义
                            printf("[MQTT] CONNACK rc=%u (0=accepted 1=bad_proto 2=bad_cid 3=server_unavail 4=bad_auth 5=not_auth)\r\n", // 打印返回码含义
                                   (unsigned)rc); // printf续行：rc值
                            if (rc == 0)                                      // 0就是连接接受
                            {                                                                 
                                got_connack = true;                           // 标记收到
                                rx_ring_consume(flen);                         // 从缓冲删掉这帧
                                break;                                        // 跳出等待循环
                            }                                                                 
                            else                                               // 返回码非0，拒绝连接
                            {                                                                 
                                // 不傻等超时，立刻退出，比如认证失败
                                printf("[MQTT] CONNECT rejected by broker (rc=%u)\r\n", (unsigned)rc); // 打印拒绝
                                rx_ring_consume(flen);                         // 删帧
                                goto conn_fail;                                // 跳连接失败
                            }                                                                 
                        }
                        // 等CONNACK时收到别的类型，意外报文
                        printf("[MQTT] unexpected msg type=%u (waiting CONNACK)\r\n", (unsigned)mtype); // 意外报文类型
                        rx_ring_consume(flen);                                 // 删掉继续等
                    }
                }
                else                                                              // 接收失败或者超时
                {                                                                 
                    // 不是超时，就是连接关闭等错误
                    if (err != ERR_TIMEOUT) // 非超时是真错误
                        printf("[MQTT] recv err=%d (timeout=%d closed=%d)\r\n", // 打印错误码
                               (int)err, (int)ERR_TIMEOUT, (int)ERR_CLSD); // printf续行：三个错误码
                    // 防止内存泄漏，有缓冲就删
                    if (nb != NULL) netbuf_delete(nb); // 有缓冲就删
                    if (err != ERR_TIMEOUT)                                      // 不是超时是真错误
                    {                                                                 
                        goto conn_fail;                                          // 连接失败
                    }                                                                 
                }                                                                 
            }                                                                
            // 循环结束还没收到，就是超时
            if (!got_connack) // 超时没收到
            {                                                                 
                // 打印CONNACK超时日志
                printf("[MQTT] CONNACK timeout (%ums, no MQTT data received)\r\n", // 打印超时日志
                       (unsigned)MQTT_CONNACK_TIMEOUT_MS); // printf续行：超时时间
                goto conn_fail; // 跳连接失败
            }                                                                 
        }
        printf("[MQTT] connected!\r\n"); // 打印已连接

        // 第五步：发订阅，等SUBACK，两个主题都成功才算在线
        pkt_len = mqtt_build_subscribe(); // 组订阅报文
        err = mqtt_write(tx_buf, pkt_len); // 发送订阅
        if (err != ERR_OK) goto conn_fail; // 失败跳连接失败

        // 单独代码块限制SUBACK变量作用域
        {
            bool got_suback = false;                                  // 有没有收到SUBACK
            bool suback_ok = false;                                  // 两个主题是不是都订阅成功
            uint32_t t0 = osKernelGetTickCount();                     // 等待开始时间
            struct netbuf *nb;                                        // 存收到的网络缓冲，mqtt_receive返回用
            // 没超时就循环
            while ((osKernelGetTickCount() - t0) < MQTT_SUBACK_TIMEOUT_MS) // 2秒超时循环
            {                                                                 
                err = mqtt_receive(&nb); // 等数据
                if (err == ERR_OK && nb != NULL) // 收到判断
                {                                                                 
                    void *dp; uint16_t dl; // 数据指针长度
                    netbuf_data(nb, &dp, &dl); // 拿数据
                    rx_ring_append((const uint8_t *)dp, dl); // 追加拼包
                    netbuf_delete(nb); // 删netbuf

                    uint8_t mtype, mflags; // 报文类型
                    const uint8_t *mbody; // 标志
                    uint32_t mbody_len; // 载荷指针
                    size_t flen = rx_ring_take_frame(&mtype, &mflags, &mbody, &mbody_len); // 载荷长度
                    if (flen > 0) // 取完整帧
                    {                                                                
                        if (mtype == 9)                                               // 类型9=SUBACK
                        {                                                                 
                            got_suback = true; // 标记收到
                            // 两个订阅两个返回码，0x80代表失败，其他成功
                            if (mbody_len >= 4) // 两个返回码判断
                            {                                                                 
                                if (mbody[2] != 0x80 && mbody[3] != 0x80) // 都不是0x80成功
                                {                                                                 
                                    suback_ok = true; // 标记成功
                                }                                                                
                                else // 否则走else
                                {                                                                 
                                    printf("[MQTT] SUBACK rejected: rc0=0x%02X rc1=0x%02X\r\n", // 打印拒绝
                                           mbody[2], mbody[3]); // printf续行：两个返回码
                                }                                                                 
                            }                                                                 
                            // 兼容只有一个返回码的broker
                            else if (mbody_len >= 3 && mbody[2] != 0x80) // 单返回码broker兼容
                            {                                                            
                                suback_ok = true; // 标记成功
                            }                                                                
                            rx_ring_consume(flen); // 消费这帧
                            break; // 跳出
                        }
                        // 其他报文直接删
                        rx_ring_consume(flen); // 其他报文删掉
                    }
                }
                // 接收失败但有缓冲，防止泄漏
                else if (nb != NULL) // 失败但有缓冲
                {                                                                
                    netbuf_delete(nb); // 删netbuf
                }                                                                
                // 真错误不是超时，连接失败
                if (err != ERR_OK && err != ERR_TIMEOUT) goto conn_fail; // 真错误跳失败
            }                                                                 
            // 没收到或者订阅失败，断开重连
            if (!got_suback || !suback_ok) // 没收到或失败
            {                                                                
                printf("[MQTT] SUBACK failed/timeout, disconnect\r\n"); // 打印失败
                goto conn_fail; // 跳连接失败
            }                                                                 
            printf("[MQTT] subscribed commands + acks\r\n"); // 打印订阅成功
        }

        // 第六步：标记连接成功，重置退避，发retain的online，再发启动事件
        g_connected = true; // 标记连接成功
        g_backoff_attempt = 0;                                           // 连接成功，退避计数清零
        // 单独块限制变量
        {
            const char *online = "online";                                // 在线状态字符串
            if (!mqtt_lock_tx()) goto conn_fail;                          // 拿锁
            // retain=true，新订阅者立刻看到在线，长度6包含所有字符
            size_t plen = mqtt_build_publish(g_topic_status, (const uint8_t *)online, 6, true, 0, 0, false); // retain=true组包
            err = mqtt_write(tx_buf, plen); // 发送
            osMutexRelease(g_tx_mutex); // 释放锁
            if (err != ERR_OK) goto conn_fail; // 失败跳

            // 发启动事件，QoS0
            MqttClient_ReportEvent("boot", "gateway started"); // 发启动事件
            printf("[MQTT] online + boot event published, entering main loop\r\n"); // 打印进主循环
        }

        // 第七步：业务主循环，收消息、心跳、补传、QoS1重发，断开就退出重连
        {
            uint32_t last_ping = osKernelGetTickCount();                // 上次心跳时间，开机初始化
            uint32_t last_replay = 0;                                   // 上次补传时间

            while (g_connected)                                         // 连接标志真就一直循环
            {                                                                 
                mqtt_pause_checkpoint();                                // 暂停检查点
                struct netbuf *nb; // 网络缓冲
                err = mqtt_receive(&nb);                        // 1秒超时等数据

                if (err == ERR_TIMEOUT)                                // 1秒到，没数据
                {                                                                 
                    // 静态变量保持上次打印时间，10秒一次存活日志
                    static uint32_t last_alive = 0; // 静态存活日志时间
                    uint32_t now_tick = osKernelGetTickCount(); // 当前tick
                    if (now_tick - last_alive >= 10000U) // 10秒一次判断
                    {                                                                 
                        printf("[MQTT] main loop alive (keepalive=%us)\r\n", (unsigned)g_cfg.keepalive_sec); // 打印存活日志
                        last_alive = now_tick; // 更新日志时间
                    }                                                                 
                    // 心跳间隔80%就发，秒转毫秒乘1000，80%就是800
                    if ((osKernelGetTickCount() - last_ping) >= (uint32_t)(g_cfg.keepalive_sec * 800)) // 80%心跳间隔判断
                    {                                                                 
                        if (!mqtt_lock_tx()) break;                                  // 拿不到锁断开
                        size_t plen = mqtt_build_pingreq();                          // 组心跳
                        err_t werr = mqtt_write(tx_buf, plen);            // 发心跳
                        osMutexRelease(g_tx_mutex); // 释放锁
                        if (werr != ERR_OK)                                          // 心跳发失败
                        {                                                                 
                            printf("[MQTT] ping write fail, disconnect\r\n"); // 打印发失败
                            break; // 断开
                        }                                                                 
                        last_ping = osKernelGetTickCount();                          // 更新上次心跳时间
                    }                                                                 

                    // 3秒间隔跑补传泵
                    if ((osKernelGetTickCount() - last_replay) >= MQTT_REPLAY_INTERVAL_MS) // 3秒补传间隔判断
                    {                                                                 
                        mqtt_replay_pump(); // 跑补传泵
                        last_replay = osKernelGetTickCount(); // 更新时间
                    }                                                                 
                    // 检查QoS1超时重发
                    mqtt_qos1_check_resend(); // 检查QoS1重发
                    continue;                                                        // 下一轮接收
                }

                // 不是超时，而且错误或者空缓冲，连接断了
                if (err != ERR_OK || nb == NULL) // 错误或空缓冲断开
                {                                                                 
                    printf("[MQTT] read err=%d, disconnect\r\n", (int)err); // 打印读错误
                    break;                                                          // 跳出业务循环
                }                                                                
                // 收到数据，拿指针长度
                void *dp; uint16_t dl; // 数据指针长度
                netbuf_data(nb, &dp, &dl); // 拿数据
                // 追加到拼包缓冲
                rx_ring_append((const uint8_t *)dp, dl); // 追加拼包
                netbuf_delete(nb); // 删netbuf

                // 无限循环切完整帧，一个TCP段可能多帧，也可能半帧
                for (;;) // 无限切帧循环
                {                                                                 
                    uint8_t mtype, mflags; // 报文类型
                    const uint8_t *mbody; // 标志
                    uint32_t mbody_len; // 载荷指针
                    size_t flen = rx_ring_take_frame(&mtype, &mflags, &mbody, &mbody_len); // 载荷长度
                    if (flen == 0) break;                                        // 没完整帧，等下一段

                    if (mtype == 3)                                              // 类型3=PUBLISH，下行数据
                    {                                                                 
                        printf("[MQTT] PUBLISH recv flags=0x%02X body=%lu\r\n", // 打印收包日志
                               (unsigned)mflags, (unsigned long)mbody_len); // printf续行：标志和长度
                        // 至少4字节才能解析主题
                        if (mbody_len >= 4) // 至少4字节
                        {                                                                 
                            // 大端2字节主题长度
                            uint16_t tlen = ((uint16_t)mbody[0] << 8) | mbody[1]; // 大端主题长度
                            if ((uint32_t)(2 + tlen) <= mbody_len) // 长度不越界
                            {                                                                 
                                const char *topic = (const char *)&mbody[2]; // 主题字符串
                                // 匹配commands主题
                                if (tlen == strlen(g_topic_commands) && // 匹配commands
                                    memcmp(topic, g_topic_commands, tlen) == 0) // memcmp比较主题
                                {                                                               
                                    mqtt_handle_command(mbody, mbody_len, mflags); // 处理命令
                                }                                                                
                                else if (tlen == strlen(g_topic_acks) && // 匹配acks
                                         memcmp(topic, g_topic_acks, tlen) == 0) // memcmp比较主题
                                {                                                                
                                }                                                                
                            }                                                                 
                        }                                                                 
                    }
                    else if (mtype == 13)                                              // 类型13=PINGRESP，心跳回复
                    {                                                                 
                        // 空实现，收到就说明连接活着，不用操作
                    }                                                                
                    else if (mtype == 4)                                               // 类型4=PUBACK，QoS1确认
                    {                                                                 
                        {                                                                
                            // 大端2字节报文ID
                            uint16_t pid = ((uint16_t)mbody[0] << 8) | mbody[1]; // 大端报文ID
                            mqtt_handle_puback(pid); // 处理PUBACK
                        }                                                                
                    }                                                                 
                    else // 其他类型
                    {                                                                 
                        // 其他类型忽略，不处理
                    }                                                                 
                    // 从缓冲删掉这帧
                    rx_ring_consume(flen); // 消费这帧
                    // 喂狗，失败跳健康失败
                    if (!mqtt_health_progress()) goto health_fail; // 喂狗
                }
            }
        }

        // 第八步：业务循环断开，清理状态，退避后重连
        (void)mqtt_connection_delete();                                  // 强转void忽略删除结果，尽力清理
        rx_ring_len = 0;                                                 // 清接收缓冲
        g_fetch_in_flight = false;                                       // 清在途取数
        g_replay_last_seq = 0;                                           // 清补传序号
        g_replay_last_tick = 0;                                          // 清补传时间
        // 块限制延时变量
        {
            uint32_t delay = mqtt_backoff_delay(g_backoff_attempt++);    // 算退避时间，次数加1
            printf("[MQTT] disconnected, reconnect in %lums (attempt=%lu)\r\n", // 打印重连日志
                   delay, (unsigned long)(g_backoff_attempt - 1)); // printf续行：延迟和次数
            if (!mqtt_delay(delay)) goto health_fail;                    // 延时失败跳健康失败
        }
        continue;                                                       // 回到主循环开头重连

    // 连接阶段失败标签，goto直接跳到这里
    conn_fail: // 连接失败标签
        (void)mqtt_connection_delete();                                  // 删失败的连接
        rx_ring_len = 0;                                                 // 清缓冲
        // 块限制变量
        {
            uint32_t delay = mqtt_backoff_delay(g_backoff_attempt++);    // 退避
            printf("[MQTT] connect failed, retry in %lums (attempt=%lu)\r\n", // 打印重试日志
                   delay, (unsigned long)(g_backoff_attempt - 1)); // printf续行：延迟和次数
            if (!mqtt_delay(delay)) goto health_fail;                    // 延时失败跳健康
        }
    }

// 健康失败标签，任务卡死，goto跳这里
health_fail: // 健康失败标签
    taskENTER_CRITICAL();                                               // 进临界区
    g_pause_acknowledged = false;                                       // 清暂停确认
    taskEXIT_CRITICAL();                                                // 退出临界区
    g_connected = false;                                                // 标记断开
    Monitor_ReportFatal(APP_MODULE_MQTT);                               // 上报致命错误
    osThreadExit();                                                     // 退出当前任务线程，不会返回
}
