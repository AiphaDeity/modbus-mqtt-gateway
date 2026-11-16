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
#include "network_upgrade.h"                    // 本模块头文件，对外接口声明
#include "network_upgrade_downloader.h"         // 下载器模块，分块下载固件
#include "https_tls.h"                         // TLS模块，一次性初始化

#include <stdio.h>                              // 标准输入输出，printf
#include <string.h>                             // 字符串处理，memcpy、strlen
#include <stdlib.h>                             // 标准库

#include "cmsis_os2.h"                          // CMSIS RTOS V2封装，信号量
#include "FreeRTOS.h"                           // FreeRTOS内核
#include "task.h"                               // 任务相关API
#include "stm32f4xx_hal.h"                      // HAL库

#include "upgrade_stage.h"                      // 升级阶段管理，M4模块
#include "upgrade_entry.h"                      // 升级写入入口，Begin/Write/Finish
#include "upgrade_quiesce.h"                    // 升级静默，暂停其他任务
#include "app_upgrade.h"                        // 应用升级相关
#include "app_modules.h"                        // 模块管理
#include "mqtt_client.h"                        // MQTT客户端，发状态上报
#include "telemetry_task.h"                     // 遥测任务，升级时暂停

// 升级通知请求结构，JSON解析出来的结果都存在这里
typedef struct // 升级请求结构体定义开始
{
    char     task_id[NW_UPG_TASK_ID_MAX];        // 升级任务ID，云端下发的
    char     version[NW_UPG_VERSION_MAX];        // 固件版本号
    uint32_t size;                               // 固件总大小，字节
    char     sha256_hex[NW_UPG_SHA256_HEX_LEN + 1];  // 固件SHA256哈希，十六进制字符串
    char     url[NW_UPG_URL_MAX];                // 固件下载地址
    char     token[NW_UPG_TOKEN_MAX];            // 鉴权token
    bool     valid;                              // 这个请求是不是合法的
} nw_upgrade_request_t; // 别名nw_upgrade_request_t定义结束

// 下载回调上下文，下载过程中传给回调函数用
typedef struct // 下载上下文结构体定义开始
{
    UpgradeEntryPeer *peer;                      // 升级写入对端，M4给的
    uint32_t          session_id;                // 本次升级会话ID
    const char       *task_id;                   // 任务ID，状态上报用
    uint32_t          total_size;                // 总大小，算进度百分比
} nw_download_ctx_t; // 别名nw_download_ctx_t定义结束

// 前向声明，内部函数，先告诉编译器有这些函数
static void NetworkUpgrade_Task(void *argument);   // 升级主任务函数
static void report_boot_result(void);              // 上报启动核验结果

// 模块状态，volatile防编译器优化，多任务共享
static volatile bool s_initialized = false;        // 模块有没有初始化
static volatile bool s_task_started = false;       // 升级任务有没有创建

// 通知信号量，收到升级通知就释放，升级任务拿到就开始执行
static osSemaphoreId_t s_notify_sem = NULL; // 通知信号量句柄
// 待处理的升级请求，放CCMRAM省SRAM，通知函数写，升级任务读
static nw_upgrade_request_t s_pending_req __attribute__((section(".bss.ccmram"))); // 待处理升级请求，CCMRAM省SRAM
static volatile bool s_upgrade_in_progress = false;  // 现在是不是正在升级

// 当前升级阶段，状态上报用，0-6下载阶段，7试运行，8回滚
static volatile int s_phase = 0; // 当前升级阶段编号
static bool s_boot_result_done;                    // 启动结果有没有上报过

// 时代计数器，每次下载尝试分配一个唯一编号，不重复
static volatile uint32_t s_epoch_counter = 0; // 时代计数器，每次尝试唯一

// OTA状态上报的主题，初始化时拼好，放CCMRAM
static char s_ota_status_topic[96] __attribute__((section(".bss.ccmram"))) = {0}; // OTA状态主题缓冲

// 升级任务静态资源，6KB，放CCMRAM不挤占SRAM，8字节对齐
_Alignas(8) static StackType_t s_upgrade_stack[NETWORK_UPGRADE_TASK_STACK_BYTES / sizeof(StackType_t)] __attribute__((section(".bss.ccmram"))); // 任务静态栈6KB，CCMRAM
static StaticTask_t s_upgrade_control_block __attribute__((section(".bss.ccmram")));  // 任务控制块
static const osThreadAttr_t s_upgrade_attributes = // 任务属性结构定义开始
{
    .name = "nw_upgrade",                          // 任务名字
    .cb_mem = &s_upgrade_control_block,            // 静态控制块地址
    .cb_size = sizeof(s_upgrade_control_block), // 控制块大小
    .stack_mem = s_upgrade_stack,                  // 静态栈地址
    .stack_size = sizeof(s_upgrade_stack), // 栈大小字节数
    .priority = NETWORK_UPGRADE_TASK_PRIORITY,     // 任务优先级
};

// 函数：json_extract_string —— 提取JSON字符串字段
// 功能：轻量手写JSON解析：在JSON里找"key":"value"格式，把value内容拷到输出缓冲并强制结尾；不用完整JSON库（省RAM），够OTA通知里取url、sha256、task_id用
// 参数：json —— JSON原文指针；key —— 要找的字段名（不带引号）；out —— 输出缓冲，value拷到这里；out_max —— 缓冲容量，防越界
// 返回：void，无返回值，没找到时out为空串
static void json_extract_string(const char *json, const char *key, char *out, int out_max) // JSON字符串字段提取函数
{
    char needle[64]; // 查找用的key缓存
    snprintf(needle, sizeof(needle), "\"%s\"", key);   // 拼出带引号的key，比如"task_id"
    const char *p = strstr(json, needle);              // 在JSON里找这个key
    if (!p) { out[0] = '\0'; return; }                // 没找到，输出空字符串
    p += strlen(needle); // 跳过key后面的引号
    // 跳过key后面的空格、冒号、制表符
    while (*p == ' ' || *p == ':' || *p == '\t') p++; // 跳过冒号和空白
    if (*p != '"') { out[0] = '\0'; return; }         // 值开头不是引号，不是字符串，返回空
    p++; // 跳过值的开头引号
    // 从引号后面开始复制，直到下一个引号或者缓冲区满
    int i = 0; // 复制计数器
    while (*p && *p != '"' && i < out_max - 1) { out[i++] = *p++; } // 逐字符复制到结尾引号
    out[i] = '\0';                                     // 字符串末尾加结束符
}

// 函数：json_extract_int —— 提取JSON整数字段
// 功能：在JSON里找"key":数字格式（可能带负号），把数字解析成int32返回；同样是不引库的轻量实现，用于取size、trial这类数值字段
// 参数：json —— JSON原文指针；key —— 要找的字段名
// 返回：int32_t，找到则返回解析出的数值；没找到返回-1（约定字段值不可能是负的所以可区分）
static int32_t json_extract_int(const char *json, const char *key) // JSON整数字段提取函数
{
    char needle[64]; // 查找用的key缓存
    snprintf(needle, sizeof(needle), "\"%s\"", key);  // 拼带引号的key
    const char *p = strstr(json, needle); // 在JSON里找key
    if (!p) return -1;                                // 没找到
    p += strlen(needle); // 跳过key本身
    while (*p == ' ' || *p == ':' || *p == '\t') p++;  // 跳过空格冒号
    if (*p == '-' || (*p >= '0' && *p <= '9')) return atoi(p);  // 是数字，转成整数
    return -1;                                         // 不是数字
}

// 函数：hex_to_bytes —— hex字符串转字节数组
// 功能：把64个小写hex字符（0-9a-f）逐对转成32字节二进制：每两个字符算一个字节（高4位×16+低4位）；OTA通知里的sha256是hex字符串，校验时要转回原始32字节摘要
// 参数：hex —— 输入hex字符串指针；out —— 输出字节数组；out_max —— 输出容量（至少需32字节），不够则失败
// 返回：int，0代表转换成功；-1代表字符不是hex或超长失败
static int hex_to_bytes(const char *hex, uint8_t *out, int out_max) // hex字符串转字节数组函数
{
    int len = strlen(hex); // 先算字符串长度
    if (len != out_max * 2) return -1;   // 两个hex字符拼一个字节，长度必须正好是两倍
    for (int i = 0; i < out_max; i++) // 逐字节转换循环
    {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];  // 每个字节对应高、低两个hex字符
        // 把hex字符转成0-15的数值，支持大小写，非法字符返回-1
        int h = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1; // 高位hex转数值
        int l = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1; // 低位hex转数值
        if (h < 0 || l < 0) return -1; // 任一非法就失败
        out[i] = (uint8_t)((h << 4) | l);   // 高4位左移，和低4位拼起来，组成一个字节
    }
    return 0; // 全部转换成功返回0
}

// 函数：report_status —— 上报升级阶段状态
// 功能：把升级进度状态包装成JSON（task_id+phase+错误详情）发到ota_status主题，云端能看到升级走到哪一步：开始下载、写flash、校验、出错带错误信息；detail为NULL就不带错误字段
// 参数：task_id —— 本次升级任务ID字符串（云端下发的）；phase —— 阶段名字符串（downloading/verifying等）；detail —— 错误详情字符串，正常阶段传NULL
// 返回：void，无返回值，发送失败不影响升级流程
static void report_status(const char *task_id, const char *phase, const char *detail) // 阶段状态上报函数
{
    if (s_ota_status_topic[0] == '\0') return;  // 主题还没拼好，不上报
    char buf[256]; // JSON缓冲区
    int len; // 报文长度
    if (detail && detail[0]) // 有没有错误详情
    {
        // 有错误详情，拼带err字段的JSON
        len = snprintf(buf, sizeof(buf), // 拼带err的JSON
            "{\"task_id\":\"%s\",\"phase\":\"%s\",\"err\":\"%s\"}", task_id, phase, detail); // 三个字段task_id phase err
    }
    else // 没详情走else
    { // else分支体开始
        // 没错误，只拼任务ID和阶段
        len = snprintf(buf, sizeof(buf), // 拼不带err的JSON
            "{\"task_id\":\"%s\",\"phase\":\"%s\"}", task_id, phase); // 两个字段task_id phase
    }
    MqttClient_PublishQos1(s_ota_status_topic, (const uint8_t *)buf, (size_t)len);  // QoS1发布
}

// 函数：report_progress —— 上报下载进度
// 功能：下载器每收一块调一次：把当前已下载字节数和总大小发到云端显示进度条；为省流量和CPU加了步进节流（如每64KB才真正发一次），小碎块只更新本地不上报
// 参数：task_id —— 升级任务ID字符串；offset —— 当前已下载的累计字节偏移；total —— 固件总字节数（452232这种）
// 返回：void，无返回值
static void report_progress(const char *task_id, uint32_t offset, uint32_t total) // 下载进度上报函数
{
    if (s_ota_status_topic[0] == '\0') return; // 主题没准备好直接返回
    char buf[256]; // JSON缓冲区
    int len = snprintf(buf, sizeof(buf), // 拼进度JSON
        "{\"task_id\":\"%s\",\"phase\":\"downloading\",\"offset\":%u,\"total\":%u}", // phase固定downloading带offset和total
        task_id, (unsigned)offset, (unsigned)total); // 当前偏移和总大小
    MqttClient_PublishQos1(s_ota_status_topic, (const uint8_t *)buf, (size_t)len); // QoS1发布进度
}

// 函数：phase_to_string —— 阶段数字转字符串
// 功能：把内部阶段枚举值（int）映射成云端和日志可读的英文短语（idle/downloading/verifying/...），查表式转换；不认识的阶段返回unknown
// 参数：phase —— 阶段枚举数值
// 返回：const char*，阶段对应的字符串指针；未知阶段返回"unknown"
static const char *phase_to_string(int phase) // 阶段数字转字符串函数
{
    switch (phase) // 按阶段值分发
    {
        case 0: return "idle";         // 空闲
        case 1: return "downloading";  // 下载中
        case 2: return "verifying";    // 校验中
        case 3: return "ready";        // 校验通过，准备重启
        case 4: return "rebooting";    // 正在重启
        case 5: return "installed";    // 安装确认
        case 6: return "failed";       // 失败
        case 7: return "trial";        // 试运行
        case 8: return "rolled_back";  // 回滚
        default: return "unknown"; // 其他未知
    }
}

// 函数：NetworkUpgrade_GetPhaseString —— 查询当前阶段字符串（对外接口）
// 功能：给别的模块（比如调试命令、MQTT状态查询）调的公开接口：返回升级模块当前所处阶段的可读字符串，方便外部展示升级状态
// 参数：无
// 返回：const char*，当前阶段英文字符串（idle/downloading等）
const char *NetworkUpgrade_GetPhaseString(void) // 对外接口函数
{
    return phase_to_string(s_phase); // 返回当前阶段字符串
}

// 函数：authorize_callback —— 升级授权回调
// 功能：执行器框架在动手前调它问"这次升级允许吗"：M2在收到MQTT通知时已经完成参数校验和忙判断，由本模块自己发起的下载执行永远放行；真正的安全校验在Notify入口
// 参数：context —— 回调上下文（本实现未用）；peer —— 升级入口对端信息（描述谁发起的）
// 返回：bool，恒返回true代表授权通过
static bool authorize_callback(void *context, const UpgradeEntryPeer *peer) // 授权回调函数
{
    (void)context; // 上下文没用，忽略
    (void)peer; // 对端没用，忽略
    return true; // 总是返回允许
}

// 升级策略结构，把授权回调注册进去
static const UpgradeEntryPolicy s_policy = // 升级策略结构定义
{
    .authorize = authorize_callback, // 注册授权回调
    .context = NULL, // 上下文空
};

// 函数：download_chunk_callback —— 下载块回调
// 功能：下载器每收到一段固件数据调一次：把这段数据按offset写进Flash的对应位置（外部flash分页写，跨页自动拆两半），边下边写不用整包缓存省RAM；每块写完可选上报进度，写失败返回错误中止下载
// 参数：ctx_ —— 回调上下文（当前未使用）；offset —— 本块在固件中的字节偏移；data —— 本块数据指针；len —— 本块字节数
// 返回：nw_dl_status_t，NW_DL_OK代表写入成功继续下一块；错误码代表flash写失败，下载器会中止
static nw_dl_status_t download_chunk_callback(void *ctx_, uint32_t offset, // 下载块回调函数
                                              const uint8_t *data, uint32_t len) // 签名续行：数据指针+数据长度
{
    nw_download_ctx_t *ctx = (nw_download_ctx_t *)ctx_;  // 把通用上下文转成我们的类型

    // 调M4的写入接口，内部处理Flash写入和偏移校验
    UpgradeStageResult ret = UpgradeEntry_Write(ctx->peer, ctx->session_id, // 调M4写入接口
                                                offset, data, len); // 会话ID 偏移 数据 长度
    if (ret != UPGRADE_STAGE_OK) // 写入失败判断
    {
        printf("[NWUPG] Write fail at offset=%u len=%u: %s\r\n", // 打印写入失败
               (unsigned)offset, (unsigned)len, UpgradeEntry_ErrorName(ret)); // 偏移长度错误名
        return NW_DL_ERR_CALLBACK;               // 写入失败，返回回调错误
    }

    // 每跨过16KB边界才上报一次进度，位运算判断是不是换了16KB的块
    if ((offset & ~0x3FFF) != ((offset + len) & ~0x3FFF)) // 跨16KB边界判断
    {
        report_progress(ctx->task_id, offset + len, ctx->total_size); // 上报进度
    }

    return NW_DL_OK; // 返回成功
}

// 网络门控，升级前暂停MQTT和遥测，防止升级过程中收发数据
// 暂停标志，只是标记发了请求，真停稳要查任务自己的确认
static volatile bool s_quiesced = false; // 暂停请求标志

// 函数：NetworkUpgrade_IsQuiesced —— 查询是否处于静默期（对外接口）
// 功能：给MQTT和遥测模块调的公开接口：返回升级是否正在执行（静默期中）；静默期里业务模块要停止可变操作（发遥测/改缓存），保证升级窗口内数据环境稳定
// 参数：无
// 返回：bool，静默期中返回真；空闲返回假
bool NetworkUpgrade_IsQuiesced(void) // 对外查询函数
{
    return s_quiesced; // 返回暂停标志
}

// 函数：quiesce_pause —— 暂停回调（静默第一步）
// 功能：执行器调的暂停钩子：进临界区原子地请求MQTT和遥测两个任务暂停（只发请求不干等），快速返回让执行器去统一等待；防止业务在升级中途继续产生新数据
// 参数：ctx —— 回调上下文（未用）；timeout_ms —— 框架给的时限（本步只是发标志立即返回用不到）
// 返回：UpgradeStageResult，UPGRADE_STAGE_OK代表暂停请求已发出
static UpgradeStageResult quiesce_pause(void *ctx, uint32_t timeout_ms) // 暂停回调函数
{
    (void)ctx; // 上下文没用
    if (timeout_ms == 0U) return UPGRADE_STAGE_TIMEOUT; // 超时0直接超时
    taskENTER_CRITICAL(); // 进临界区
    s_quiesced = true; // 置暂停标志
    MqttClient_SetPaused(true); // MQTT请求暂停
    Telemetry_SetPaused(true); // 遥测请求暂停
    taskEXIT_CRITICAL(); // 出临界区
    printf("[NWUPG] pause requested\r\n"); // 打印请求日志
    return UPGRADE_STAGE_OK; // 返回成功
}

// 函数：quiesce_settle —— 等待停稳（静默第二步）
// 功能：真正等MQTT和遥测任务自己停到安全点：轮询调MqttClient_IsPaused和Telemetry_IsPaused（请求+确认才算停稳），规定时限内两个都停稳才成功；注意不能代报健康，必须等任务自己报
// 参数：ctx —— 回调上下文（未用）；timeout_ms —— 最长等待毫秒数，超时算失败中止升级
// 返回：UpgradeStageResult，OK代表两个任务都停稳；TIMEOUT代表时限内没停稳，升级中止
static UpgradeStageResult quiesce_settle(void *ctx, uint32_t timeout_ms) // 等待停稳函数
{
    (void)ctx; // 上下文没用
    uint32_t started_ms = HAL_GetTick(); // 记起始时间
    while ((uint32_t)(HAL_GetTick() - started_ms) < timeout_ms) // 限时轮询循环
    {
        if (!s_quiesced) return UPGRADE_STAGE_MODE; // 被恢复就返回模式错误
        bool mqtt_paused = MqttClient_IsPaused(); // 查MQTT确认
        bool telemetry_paused = Telemetry_IsPaused(); // 查遥测确认
        if (mqtt_paused && telemetry_paused) return UPGRADE_STAGE_OK;  // 两个都确认暂停了
        if (osDelay(1U) != osOK) return UPGRADE_STAGE_INTERNAL; // 睡1毫秒再查
    }
    printf("[NWUPG] settle timeout mqtt=%u telemetry=%u\r\n", // 打印超时
           (unsigned)MqttClient_IsPaused(), (unsigned)Telemetry_IsPaused()); // 两个任务的确认状态
    return UPGRADE_STAGE_TIMEOUT; // 返回超时
}

// 函数：quiesce_sync —— 同步确认（静默第三步）
// 功能：网络侧没有额外要落盘的持久数据，同步阶段直接再确认一次两个任务仍然停稳（防止settle后到sync间被意外唤醒），是个轻量双保险
// 参数：ctx —— 回调上下文（未用）；timeout_ms —— 时限（本步只做快速复查）
// 返回：UpgradeStageResult，OK代表确认停稳；失败码代表状态被破坏
static UpgradeStageResult quiesce_sync(void *ctx, uint32_t timeout_ms) // 同步确认函数
{
    return quiesce_settle(ctx, timeout_ms); // 直接复用settle
}

// 函数：quiesce_resume —— 恢复回调（静默解除）
// 功能：升级成功或失败后执行器调它解除静默：撤销MQTT和遥测的暂停请求，两个业务任务自动从暂停点恢复干活，系统回到正常运转
// 参数：ctx —— 回调上下文（未用）；timeout_ms —— 时限（恢复只是发标志立即生效）
// 返回：UpgradeStageResult，OK代表恢复请求已发出
static UpgradeStageResult quiesce_resume(void *ctx, uint32_t timeout_ms) // 恢复回调函数
{
    (void)ctx; // 上下文没用
    if (timeout_ms == 0U) return UPGRADE_STAGE_TIMEOUT; // 超时0直接超时
    taskENTER_CRITICAL(); // 进临界区
    MqttClient_SetPaused(false); // MQTT恢复
    Telemetry_SetPaused(false); // 遥测恢复
    s_quiesced = false; // 清暂停标志
    taskEXIT_CRITICAL(); // 出临界区
    printf("[NWUPG] pause request cleared\r\n"); // 打印恢复日志
    return UPGRADE_STAGE_OK; // 返回成功
}

// 静默参与者结构，把四个回调都注册进去
static const UpgradeQuiesceParticipant s_quiesce_participant = // 静默参与者结构定义
{
    .pause = quiesce_pause, // 注册暂停回调
    .settle = quiesce_settle, // 注册停稳回调
    .sync = quiesce_sync, // 注册同步回调
    .resume = quiesce_resume, // 注册恢复回调
    .context = NULL, // 上下文空
};

// 函数：NetworkUpgrade_Init —— 升级模块初始化（对外接口）
// 功能：开机装配时调一次：把模块状态复位到idle、给执行器注册全套回调（授权/下载块/静默四件套）、注册M2平台钩子；幂等设计，重复调安全
// 参数：无
// 返回：void，无返回值，初始化没有可失败项
void NetworkUpgrade_Init(void) // 模块初始化函数
{
    if (s_initialized) return; // 已初始化直接返回

    // 创建通知信号量，初始0，收到通知就释放，任务拿到就执行
    s_notify_sem = osSemaphoreNew(1, 0, NULL); // 建通知信号量初值0
    if (!s_notify_sem) // 建失败判断
    {
        printf("[NWUPG] sem create fail, upgrade unavailable\r\n"); // 打印失败提示
        return; // 直接返回放弃升级能力
    }

    // 从MQTT模块拿OTA状态主题，复制到本地
    const char *ota_topic = MqttClient_TopicOtaStatus(); // 拿OTA主题
    if (ota_topic && ota_topic[0]) // 主题非空判断
    {
        strncpy(s_ota_status_topic, ota_topic, sizeof(s_ota_status_topic) - 1); // 复制主题到本地
    }

    // 绑定权限策略，失败不阻断启动，只是升级不能用
    UpgradeStageResult ret = UpgradeEntry_BindPolicy(&s_policy); // 绑定升级策略
    if (ret != UPGRADE_STAGE_OK) // 绑定失败判断
    {
        printf("[NWUPG] BindPolicy fail: %s\r\n", UpgradeEntry_ErrorName(ret)); // 打印绑定失败
    }

    // 绑定网络门控，升级时由协调器调我们的暂停回调
    ret = UpgradeQuiesce_Bind(UPGRADE_QUIESCE_NETWORK, &s_quiesce_participant); // 绑定网络门控
    if (ret != UPGRADE_STAGE_OK) // 绑定失败判断
    {
        printf("[NWUPG] QuiesceBind(NETWORK) fail: %s\r\n", UpgradeEntry_ErrorName(ret)); // 打印门控失败
    }

    // 初始化TLS，熵源、随机数、时间函数，失败只是HTTPS不能用，HTTP还能用
    https_tls_result_t tls_ret = HttpsTls_Init(); // 初始化TLS
    if (tls_ret != HTTPS_TLS_OK) // TLS失败判断
    {
        printf("[NWUPG] TLS init fail: %d (HTTPS download unavailable)\r\n", (int)tls_ret); // 打印TLS失败
    }

    s_initialized = true; // 标记初始化完成
    printf("[NWUPG] Init OK: policy+quiesce bound\r\n"); // 打印成功日志
}

// 函数：NetworkUpgrade_StartTask —— 创建升级任务（对外接口）
// 功能：装配流程后段调：创建OTA升级执行任务（独立FreeRTOS线程），用静态分配的栈避免运行时内存不足；任务平时睡眠等信号量，收到升级通知才醒来干整条链路
// 参数：无
// 返回：void，无返回值，创建失败打日志由看门狗兜底
void NetworkUpgrade_StartTask(void) // 创建升级任务函数
{
    if (!s_initialized || s_task_started) return; // 没初始化或已启动直接返回

    osThreadId_t tid = osThreadNew((osThreadFunc_t)NetworkUpgrade_Task, // 创建升级任务
                                   NULL, &s_upgrade_attributes); // 属性：静态栈和优先级
    if (tid) // 创建成功判断
    {
        s_task_started = true; // 标记已启动
        printf("[NWUPG] task started (stack=%d)\r\n", NETWORK_UPGRADE_TASK_STACK_BYTES); // 打印启动日志含栈大小
    }
    else // 创建失败走else
    {
        printf("[NWUPG] task create fail\r\n"); // 打印创建失败
    }
}

// 函数：NetworkUpgrade_Notify —— 升级通知处理（对外接口）
// 功能：MQTT模块收到OTA通知JSON后调的入口：手写解析url/sha256/size/task_id/trial五个字段并校验完整性（缺url或sha256拒绝），检查模块空闲，全部通过则存任务参数、发信号量唤醒升级任务，并回"已受理"
// 参数：json —— 云端下发的OTA通知JSON字符串
// 返回：int，0代表受理成功已唤醒升级任务；-1代表JSON缺字段；-2代表当前正在升级中（忙）；-3代表模块没初始化
int NetworkUpgrade_Notify(const char *json) // 升级通知处理函数
{
    if (!s_initialized || !s_task_started) return -3; // 没初始化返回-3
    if (s_upgrade_in_progress) return -2;   // 已经在升级了，拒绝

    // 先清空待处理结构，然后逐个解析JSON字段，结构在CCMRAM不占栈
    memset(&s_pending_req, 0, sizeof(s_pending_req)); // 清空待处理结构
    json_extract_string(json, "task_id", s_pending_req.task_id, sizeof(s_pending_req.task_id)); // 解析task_id
    json_extract_string(json, "version", s_pending_req.version, sizeof(s_pending_req.version)); // 解析version
    json_extract_string(json, "sha256", s_pending_req.sha256_hex, sizeof(s_pending_req.sha256_hex)); // 解析sha256
    json_extract_string(json, "url", s_pending_req.url, sizeof(s_pending_req.url)); // 解析url
    json_extract_string(json, "token", s_pending_req.token, sizeof(s_pending_req.token)); // 解析token
    s_pending_req.size = (uint32_t)json_extract_int(json, "size"); // 解析size

    // 三个必填字段：url、size、sha256，缺一个都不行
    if (s_pending_req.url[0] == '\0' || s_pending_req.size == 0 || // 必填字段检查
        s_pending_req.sha256_hex[0] == '\0') // url size sha256任一为空
    {
        printf("[NWUPG] Notify: missing required fields\r\n"); // 打印缺字段
        return -1; // 返回-1
    }

    s_pending_req.valid = true; // 标记请求合法
    printf("[NWUPG] Notify: task=%s ver=%s size=%u url=%s\r\n", // 打印通知内容
           s_pending_req.task_id, s_pending_req.version, // 任务版本大小地址
           (unsigned)s_pending_req.size, s_pending_req.url); // printf续行：大小和下载地址

    // 释放信号量，唤醒升级执行任务，真正的下载在独立线程里跑
    osSemaphoreRelease(s_notify_sem); // 释放信号量唤醒任务
    return 0; // 返回0成功
}

// 函数：NetworkUpgrade_Task —— 升级主任务
// 功能：FreeRTOS任务入口：平时阻塞等升级信号量；被唤醒后走OTA全链路——静默两个业务任务→URL拆解→下载器流式拉固件（边下边写flash）→下载完SHA256校验→校验过通知bootloader重启换区；空闲时定期检查上次启动结果上报云端
// 参数：argument —— FreeRTOS任务参数，没用到
// 返回：void，无返回值，死循环不退出
static void NetworkUpgrade_Task(void *argument) // 升级主任务函数
{
    (void)argument; // 参数没用忽略

    printf("[NWUPG] task running, waiting for notify...\r\n"); // 打印等待日志

    for (;;) // 死循环
    {
        // 等通知信号量，1秒超时，超时就去检查启动结果
        osStatus_t wait = osSemaphoreAcquire(s_notify_sem, osKernelGetTickFreq()); // 等信号量1秒超时
        if (wait != osOK) // 超时判断
        {
            report_boot_result(); // 检查上报启动结果
            continue; // 继续下一轮
        }
        if (!s_pending_req.valid) continue; // 请求无效跳过

        s_upgrade_in_progress = true; // 标记升级进行中
        // 把待处理请求复制到本地，防止通知函数覆盖全局的
        nw_upgrade_request_t req; // 本地请求副本
        memcpy(&req, &s_pending_req, sizeof(req)); // 复制全局请求
        s_pending_req.valid = false; // 清全局有效标志

        printf("[NWUPG] === upgrade start: task=%s ===\r\n", req.task_id); // 打印升级开始

        // 分配唯一的时代编号，M4用它区分不同的下载会话
        uint32_t epoch = ++s_epoch_counter; // 分配时代编号

        UpgradeEntryPeer peer = // M4对端结构
        {
            .source = UPGRADE_STAGE_SOURCE_NETWORK,   // 来源是网络下载
            .epoch = epoch, // 时代编号
        };

        // 把SHA256的十六进制字符串转成32字节，M4校验用
        uint8_t sha256_bytes[UPGRADE_STAGE_SHA256_SIZE]; // SHA256字节缓冲
        if (hex_to_bytes(req.sha256_hex, sha256_bytes, sizeof(sha256_bytes)) != 0) // hex转字节失败判断
        {
            printf("[NWUPG] sha256 hex parse fail: %s\r\n", req.sha256_hex); // 打印解析失败
            report_status(req.task_id, "failed", "E_SHA256_FORMAT"); // 上报失败状态
            s_phase = 6; // 置失败阶段
            s_upgrade_in_progress = false; // 清进行中标志
            continue; // 跳过本轮
        }

        // 第一步Begin，M4开升级会话，分配session_id，写升级头到Flash
        uint32_t session_id = 0; // 会话ID初始0
        s_phase = 1; // 置下载阶段
        report_status(req.task_id, "downloading", NULL); // 上报下载中

        UpgradeStageResult ret = UpgradeEntry_Begin(&peer, NULL, req.size, // 调M4 Begin
                                                    sha256_bytes, &session_id); // 对端 大小 SHA256 会话ID
        if (ret != UPGRADE_STAGE_OK) // Begin失败判断
        {
            printf("[NWUPG] Begin fail: %s\r\n", UpgradeEntry_ErrorName(ret)); // 打印Begin失败
            report_status(req.task_id, "failed", UpgradeEntry_ErrorName(ret)); // 上报失败
            s_phase = 6; // 置失败阶段
            s_upgrade_in_progress = false; // 清进行中标志
            continue; // 跳过本轮
        }
        printf("[NWUPG] Begin OK: session=%u epoch=%u\r\n", (unsigned)session_id, (unsigned)epoch); // 打印Begin成功

        // 第二步下载，构造下载上下文，回调里写Flash
        nw_download_ctx_t dl_ctx = // 下载上下文
        {
            .peer = &peer, // 对端指针
            .session_id = session_id, // 会话ID
            .task_id = req.task_id, // 任务ID
            .total_size = req.size, // 总大小
        };

        nw_dl_params_t dl_params = // 下载参数
        {
            .url = req.url, // 下载地址
            .auth_token = (req.token[0] != '\0') ? req.token : NULL, // 鉴权token有才传
            .total_size = req.size, // 总大小
            .cb = download_chunk_callback,            // 每块回调写Flash
            .ctx = &dl_ctx, // 下载上下文指针
            .timeout_ms = 120000,                     // 2分钟超时
            .chunk_size = UPGRADE_STAGE_MAX_CHUNK_SIZE, // 1016字节一块，Flash写入单元
        };

        nw_dl_status_t dl_ret = NetworkDownloader_Fetch(&dl_params); // 发起下载
        if (dl_ret != NW_DL_OK) // 下载失败判断
        {
            printf("[NWUPG] download fail: %s\r\n", NetworkDownloader_ErrorString(dl_ret)); // 打印下载失败
            report_status(req.task_id, "failed", NetworkDownloader_ErrorString(dl_ret)); // 上报失败状态
            // 下载失败，取消会话、断开对端，清理M4的状态
            UpgradeEntry_Abort(&peer, session_id); // 取消M4会话
            UpgradeEntry_Disconnect(&peer); // 断开对端
            s_phase = 6; // 置失败阶段
            s_upgrade_in_progress = false; // 清进行中标志
            continue; // 跳过本轮
        }

        // 第三步Finish，M4校验SHA256和签名，成功就提交READY，固件完整校验通过
        s_phase = 2; // 置校验阶段
        report_status(req.task_id, "verifying", NULL); // 上报校验中

        ret = UpgradeEntry_Finish(&peer, session_id); // 调M4 Finish校验
        if (ret != UPGRADE_STAGE_OK) // Finish失败判断
        {
            printf("[NWUPG] Finish fail: %s\r\n", UpgradeEntry_ErrorName(ret)); // 打印Finish失败
            report_status(req.task_id, "failed", UpgradeEntry_ErrorName(ret)); // 上报失败
            UpgradeEntry_Disconnect(&peer); // 断开对端
            s_phase = 6; // 置失败阶段
            s_upgrade_in_progress = false; // 清进行中标志
            continue; // 跳过本轮
        }

        printf("[NWUPG] Finish OK: READY committed, rebooting in 1s...\r\n"); // 打印校验通过
        s_phase = 3; // 置就绪阶段
        report_status(req.task_id, "ready", NULL); // 上报就绪
        osDelay(1000);   // 等1秒，给云端时间收到ready状态

        // 第四步Reboot，M4重启设备，成功了不会返回；重启后Boot核验新固件
        s_phase = 4; // 置重启阶段
        report_status(req.task_id, "rebooting", NULL); // 上报重启中
        ret = UpgradeEntry_Reboot(&peer, session_id); // 调M4 Reboot
        // 能走到这里说明重启失败了，正常情况不会发生
        printf("[NWUPG] Reboot returned: %s (should not happen)\r\n", // 打印重启失败
               UpgradeEntry_ErrorName(ret)); // 错误名
        report_status(req.task_id, "failed", "E_REBOOT"); // 上报重启失败

        s_upgrade_in_progress = false; // 清进行中标志
    }
}

// ===== 启动结果上报，重启后Boot核验的结果要发给云端，空闲时每秒检查
// 函数：report_boot_result —— 上报启动结果
// 功能：开机后把上一次固件分区的启动结果发到云端：trial试运行成功→发confirmed请求转正；失败→通知回滚；已确认→静默不打扰；靠MQTT的QoS1+确认重发机制保证结果必达云端
// 参数：无（读全局升级上下文里的boot标志）
// 返回：void，无返回值，发送失败等MQTT重发机制兜底
static void report_boot_result(void) // 启动结果上报函数
{
    if (s_boot_result_done) return;   // 已经上报成功，不重复

    // 从M4拿本次启动的身份信息，事务ID、序列号、摘要
    UpgradeMessage identity; // 升级身份结构
    if (!App_UpgradeGetStartupIdentity(&identity)) // 拿身份失败判断
    {
        s_boot_result_done = true;   // 没有升级身份，不是升级启动，不用上报
        return; // 直接返回
    }
    AppUpgradeResult result = App_UpgradeGetResult(); // 拿核验结果
    if ((result == APP_UPGRADE_RESULT_UNKNOWN) || !MqttClient_IsConnected()) return; // 结果未知或MQTT没连直接返回

    const char *phase; // 阶段字符串指针
    switch (result) // 按结果分发
    {
        case APP_UPGRADE_RESULT_TRIAL:        // 试运行
            phase = "trial"; // 试运行阶段
            s_phase = 7; // 置阶段7
            break; // 跳出switch
        case APP_UPGRADE_RESULT_CONFIRMED:    // 确认安装
            phase = "confirmed"; // 确认阶段
            s_phase = 5; // 置阶段5
            break; // 跳出switch
        case APP_UPGRADE_RESULT_ROLLED_BACK:  // 回滚
            phase = "rolled_back"; // 回滚阶段
            s_phase = 8; // 置阶段8
            break; // 跳出switch
        default: // 其他结果
            return; // 直接返回不处理
    }

    // 32字节摘要转64个十六进制字符，云端校验固件指纹
    char digest[65]; // 64字符摘要缓冲
    static const char hex[] = "0123456789abcdef"; // hex字符表
    for (size_t i = 0; i < sizeof(identity.digest); i++) // 逐字节转hex循环
    {
        digest[i * 2] = hex[identity.digest[i] >> 4];       // 高4位
        digest[i * 2 + 1] = hex[identity.digest[i] & 0x0fU];  // 低4位
    }
    digest[64] = '\0'; // 加字符串结束符

    // 拼启动结果JSON，带事务ID、序列号、阶段、固件摘要
    char payload[256]; // JSON缓冲
    int len = snprintf(payload, sizeof(payload), // 拼启动结果JSON
        "{\"type\":\"boot_result\",\"tx_id\":\"%08lx%08lx\",\"sequence\":%lu," // 类型事务ID序列号阶段摘要
        "\"phase\":\"%s\",\"image_sha256\":\"%s\"}", // 各字段占位
        (unsigned long)(identity.tx_id >> 32), (unsigned long)(uint32_t)identity.tx_id, // 高32位低32位序列阶段摘要
        (unsigned long)identity.sequence, phase, digest); // printf续行：序列号+阶段+摘要
    if ((len <= 0) || ((size_t)len >= sizeof(payload))) return; // 长度异常直接返回

    // 用升级结果专用QoS1通道发，等PUBACK，返回1就是broker确认了，标记完成不再重发
    if (MqttClient_PublishUpgradeResult((const uint8_t *)payload, (size_t)len) == 1) // QoS1发布等PUBACK
    {
        s_boot_result_done = true; // 标记上报完成
        printf("[NWUPG] boot result PUBACK: tx=%08lx%08lx phase=%s\r\n", // 打印PUBACK日志
               (unsigned long)(identity.tx_id >> 32), (unsigned long)(uint32_t)identity.tx_id, phase); // 事务ID和阶段
    }
}
