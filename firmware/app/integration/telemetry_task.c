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

#include <stdio.h>                          // C标准输入输出库，提供 snprintf 字符串格式化函数
#include <string.h>                         // C标准字符串库，提供 memset、memcpy 等函数

#include "FreeRTOS.h"                       // FreeRTOS 内核主头文件，任务、队列都靠它
#include "task.h"                           // FreeRTOS 任务相关 API，临界区宏在这里
#include "cmsis_os2.h"                      // ARM CMSIS-RTOS2 封装层，统一的RTOS接口

#include "mb_api.h"                         // M1 Modbus 主接口，遍历从站、拿快照都用它
#include "mb_pointmap.h"                    // M1 点表转换，把原始寄存器值换成物理量
#include "storage_m2.h"                     // M3 存储模块给M2的接口，写离线缓存队列
#include "storage_seq.h"                    // M3 全局序列号分配，每条数据一个唯一编号
#include "mqtt_client.h"                    // M2 自己的MQTT客户端，在线时直接发数据
#include "app_modules.h"                    // 应用模块装配，拿各个任务句柄和队列
#include "app_monitor.h"                    // 应用监控，上报任务状态和故障
#include "upgrade_quiesce.h"                // OTA升级静默接口，升级时让遥测任务暂停

// 遥测上报周期，单位毫秒，2秒一次；Modbus采集本身1-2秒一轮，2秒刚好
#define TELEMETRY_PERIOD_MS   2000U           // 遥测周期2秒，2秒上报一轮

// 单条JSON报文最大长度，和M3存储的上限对齐，16个点的完整报文大概768字节
#define PAYLOAD_MAX           768U            // 单条JSON最大768字节，和M3存储上限对齐

// 最多收集多少个从站ID，硬件上限是16个，和M1的 MB_SLAVE_HARD_MAX 对齐
#define TEL_MAX_SLAVES        16U             // 从站ID列表最多16个，和M1硬件上限对齐

// 暂停请求标志：别的任务请求遥测暂停时置位，volatile防止编译器优化
static volatile bool s_pause_requested;    // 暂停请求标志，volatile防优化
// 暂停确认标志：遥测任务自己真的停稳了才置位，请求方靠这个判断是否真停了
static bool s_pause_acknowledged;         // 暂停确认标志，任务停稳才置位

// 函数：Telemetry_SetPaused —— 请求遥测暂停/恢复（对外接口）
// 功能：给OTA升级模块等调的公开接口：进临界区原子地设置暂停请求标志，只置请求不直接动确认标志，必须等遥测任务自己跑到安全点停稳后置确认，防止暂停瞬间有半途的采集/发送
// 参数：paused —— 真=请求遥测任务暂停（采完当前从站就停）；假=恢复采集
// 返回：void，无返回值，真停与否用Telemetry_IsPaused查询
void Telemetry_SetPaused(bool paused)         // 对外接口：请求遥测任务暂停/恢复
{
    taskENTER_CRITICAL();                        // 进入临界区，关中断，防止读写过程中被打断
    s_pause_requested = paused;                 // 把请求标志设成传入的值（true=暂停，false=恢复）
    s_pause_acknowledged = false;               // 先把旧确认清掉，等任务自己处理完再重新确认
    taskEXIT_CRITICAL();                         // 退出临界区，开中断
}

// 函数：Telemetry_IsPaused —— 查询是否真暂停（对外接口）
// 功能：给升级模块调的公开接口：暂停请求和遥测任务自己置的确认标志都为真才算真停稳，缺一不算，保证升级模块动flash时遥测绝不在采集/发送中间态
// 参数：无
// 返回：bool，真暂停（请求+确认都为真）返回真；没请求或还没停稳返回假
bool Telemetry_IsPaused(void)                 // 对外接口：查询遥测是不是真的停稳了
{
    taskENTER_CRITICAL();                        // 进临界区读标志，保证读到的是完整值
    bool paused = s_pause_requested && s_pause_acknowledged;  // 两个标志都为真才算真暂停
    taskEXIT_CRITICAL();                         // 退出临界区
    return paused;                               // 返回结果给调用方
}

// ===== 阶段1：收集有效从站ID，回调里只读ID，不调任何加锁函数，防止自死锁 =====

// 临时存从站ID列表的结构体，遍历M1从站时用
typedef struct                               // 从站ID列表结构体定义开始
{
    uint8_t ids[TEL_MAX_SLAVES];                 // 存从站ID的数组，最多16个
    uint8_t n;                                    // 当前已经收集了多少个ID
} tel_slave_list_t;                           // 别名tel_slave_list_t定义结束

// 函数：collect_slave_ids —— 从站遍历回调（阶段1）
// 功能：M1模块遍历在线从站时每找到一个有效从站就调一次本回调：把从站ID追加到上下文的ID数组里并计数，先纯收集不动数据，为阶段2逐个处理准备清单
// 参数：slave —— M1传来的从站运行时结构指针，里面有从站ID等信息；ctx —— 回调上下文指针，实际是ID收集数组的地址，收集结果存在这里
// 返回：int，0代表正常继续遍历下一个从站；非0代表终止遍历（本函数恒返回0）
static int collect_slave_ids(mb_slave_runtime_t *slave, void *ctx)   // 阶段1回调：只收集从站ID
{
    tel_slave_list_t *list = (tel_slave_list_t *)ctx;  // 把传进来的上下文转成ID列表指针
    if (list->n < TEL_MAX_SLAVES)             // 列表还没满16个才存
    {
        // 只记ID，不读数据字段，数据字段等阶段2锁释放了再用快照安全读取
        list->ids[list->n++] = slave->telegram.u8id;  // 把从站ID存进数组，然后计数加1
    }
    return 0;                                      // 返回0告诉M1继续遍历，不要停
}

// ===== 阶段2：处理单个从站，此时遍历的锁已经释放，可以安全调任意接口 =====

// 函数：publish_one_slave —— 处理单个从站全流程（阶段2）
// 功能：单个从站的完整数据链路：向M1请求该从站寄存器快照→用快照里的点表定义拼遥测JSON（含RTC时间戳）→写M3缓存队列（离线也能补传）→MQTT在线时同时直发；升级静默期跳过发送只存缓存
// 参数：slave_id —— 待处理的Modbus从站地址（1-247）
// 返回：bool，true代表正常处理完（含主动跳过）；false代表监控看门狗已超时，调用方应停止本轮采集
static bool publish_one_slave(uint8_t slave_id)   // 阶段2：处理单个从站，取快照拼JSON存储发送
{
    // 1) 取快照：M1内部加锁，安全读寄存器数据和有效标志
    mb_slave_snapshot_t snap;                     // 存从站快照的结构体
    if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 3000U)) return false;  // 告诉看门狗我要开始等，3秒超时
    mb_status_e snapshot_status = mb_take_slave_snapshot(slave_id, &snap);  // 从M1拿这个从站的数据快照
    if (!Monitor_ReportProgress(APP_MODULE_TELEMETRY, true)) return false;  // 喂狗，报告我还活着
    if (snapshot_status != MB_STATUS_OK)       // 快照状态不是OK
        return true;                              // 拿快照失败（比如从站不存在），直接返回正常，不报错

    // 数据无效或者一个有效点都没有，就跳过这个从站
    if (!snap.data_valid || snap.valid_point == 0)   // 数据无效或没有效点
    {
        printf("[TEL] skip slave=%u dv=%u vp=%u\r\n",
               (unsigned)slave_id, (unsigned)snap.data_valid,
               (unsigned)snap.valid_point);       // 打印跳过原因，方便调试
        return true;                              // 跳过不算错误，返回正常
    }

    // 2) 解析点位表：M1内部加锁定位配置、读原始寄存器，此时本任务不持有任何Modbus锁，安全
    // static 让数组只初始化一次，__attribute__ 把它放到CCMRAM里，省主SRAM
    static mb_parsed_point_t pts[MB_POINTS_PER_SLAVE] __attribute__((section(".bss.ccmram")));
    if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 3000U)) return false;  // 喂狗，开始等解析
    int npts = mb_pointmap_parse_slave(slave_id, pts);  // M1把原始寄存器值转成带物理量的点位
    if (!Monitor_ReportProgress(APP_MODULE_TELEMETRY, true)) return false;  // 喂狗，解析完了
    if (npts <= 0)                              // 解析出0个或负数个点
    {
        printf("[TEL] no points slave=%u\r\n", (unsigned)slave_id);  // 一个点都没解析出来
        return true;                              // 不算错误，跳过
    }

    // 3) 拼JSON报文，格式：{"g":"网关ID","slave":从站号,"online":在线状态,"pts":[点位数组]}
    // 缓冲区也放CCMRAM，省主SRAM，static只初始化一次
    static char buf[PAYLOAD_MAX] __attribute__((section(".bss.ccmram")));
    // 先写JSON头部，n记录当前写到了第几个字节
    int n = snprintf(buf, sizeof(buf),
        "{\"g\":\"mb2mqtt_001\",\"slave\":%u,\"online\":%u,\"pts\":[",
        (unsigned)slave_id, (unsigned)snap.is_online);

    // 循环每个点位，把有效点拼进pts数组
    for (int i = 0; i < npts; i++)             // 遍历解析出的所有点位
    {
        if (!pts[i].valid) continue;              // 无效点直接跳过
        // 在当前位置接着写一个点位对象，n是点位名，v是数值，u是单位
        int w = snprintf(buf + n, sizeof(buf) - n,
            "%s{\"n\":\"%s\",\"v\":%.2f,\"u\":\"%s\"}",
            (n > 0 && buf[n-1] != '[') ? "," : "",  // 不是第一个点就加逗号，第一个点不加
            pts[i].name, (double)pts[i].value, pts[i].unit);
        if (n + w >= (int)sizeof(buf) - 2) break;  // 缓冲区快满了就停，防止溢出
        n += w;                                   // 更新写入位置
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");  // 收尾，闭合pts数组和整个JSON对象

    // 4) 从M3拿全局序列号，每条数据一个唯一编号，离线补传时靠它去重排序
    uint64_t seq = 0;                          // 全局序列号，先清0
    if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 3000U)) return false;  // 喂狗
    HAL_StatusTypeDef sequence_status = storage_seq_alloc(&seq);  // M3分配序列号
    if (!Monitor_ReportProgress(APP_MODULE_TELEMETRY, true)) return false;  // 喂狗
    if (sequence_status != HAL_OK)             // 序列号分配结果不是OK
    {
        printf("[TEL] seq_alloc failed, skip slave=%u\r\n", (unsigned)slave_id);  // 序列号分配失败
        return true;                              // 跳过不算错误
    }

    // 5) 填存储请求，丢给M3的离线缓存队列，断网时M3会存到Flash，联网了再补传
    storage_write_req_t req = {
        .kind = 1,                                // 记录类型：1=遥测数据
        .seq = seq,                               // 刚拿到的全局序列号
        .captured_at_ms = 0,                      // 采集时间戳，这里先填0
        .payload_len = (uint16_t)n,               // JSON报文长度
    };
    memcpy(req.payload, buf, n);                   // 把拼好的JSON拷进请求体

    taskENTER_CRITICAL();                          // 进临界区检查能不能发
    // 没被暂停、OTA也没让静默，才允许写数据
    bool allowed = !s_pause_requested && UpgradeQuiesce_AllowsMutation();
    // 允许就把请求丢进M3的写队列，不允许就返回资源错误
    osStatus_t st = allowed ? osMessageQueuePut(storage_m2_write_queue(), &req, 0, 0) : osErrorResource;
    taskEXIT_CRITICAL();                           // 退出临界区
    if (!allowed) return true;                     // 被暂停了就正常返回，不算错误
    if (st != osOK)                            // M3队列没接收成功
    {
        printf("[TEL] cache not accepted, skip publish slave=%u status=%d\r\n",
               (unsigned)slave_id, (int)st);      // 队列满了或者出错，M3没接收
        return true;                              // 跳过，不影响下一轮
    }

    // 6) 在线时直接通过MQTT发出去，M3队列只是离线备份，不管队列满不满都发
    printf("[TEL-DATA] %.*s\r\n", (int)n, buf);    // 打印完整JSON报文，方便调试
    // 发送锁最多12秒、写入最多10秒，留24秒余量，防止看门狗误触发
    if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 24000U)) return false;  // 喂狗，开始等发送
    int pr = MqttClient_Publish(MqttClient_TopicTelemetry(),
                               (const uint8_t *)buf, (size_t)n);  // 往遥测主题发JSON
    if (!Monitor_ReportProgress(APP_MODULE_TELEMETRY, true)) return false;  // 喂狗，发送完了
    printf("[TEL] seq=%lu slave=%u online=%u pts=%d mqtt_pub=%d\r\n",
           (unsigned long)seq, (unsigned)slave_id,
           (unsigned)snap.is_online, npts, pr);    // 打印这一条的处理结果
    return true;                                   // 正常处理完了
}

// 函数：Telemetry_Init —— 遥测模块初始化（对外接口）
// 功能：开机装配调用的空初始化：遥测任务自己不建硬件资源、不开内核对象（快照和队列都是M1/M3的），直接返回成功，保证装配流程统一
// 参数：无
// 返回：AppModuleInitResult，恒返回APP_MODULE_INIT_OK代表成功
AppModuleInitResult Telemetry_Init(void)      // 对外接口：空初始化，没硬件资源要建
{
    return APP_MODULE_INIT_OK;                     // 直接返回初始化成功
}

/* 采集、合法空闲及暂停检查由本任务报告；超时不继续恢复健康状态。 */
// 函数：Telemetry_Task —— 遥测主任务
// 功能：FreeRTOS任务入口，跑采集大循环：先等M1/M3就绪并扫一遍全部从站建ID清单（阶段1），然后周期轮询：每个从站调publish_one_slave采数据存缓存发MQTT（阶段2），全部处理完一轮睡固定间隔再来；支持暂停检查点（升级时停稳）、监控健康上报、看门狗超时退出
// 参数：argument —— FreeRTOS任务参数，本任务没用到，传NULL
// 返回：void，无返回值，死循环不退出，只有看门狗健康失败才break退出任务
void Telemetry_Task(void *argument)            // 遥测主任务，FreeRTOS创建后死循环跑
{
    (void)argument;                               // 没用到任务参数，显式告诉编译器别报警告
    // 注册健康监控：2秒喂一次狗，30秒没喂就判定任务卡死
    if (!Monitor_RegisterHealth(APP_MODULE_TELEMETRY, 2000U, 30000U))  // 注册健康监控失败判断
        goto health_fail;                         // 注册失败直接跳错误处理
    // 等系统初始化完（DHCP、MQTT、Modbus都起来）再开始，先等5秒
    if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 6000U) ||   // 登记6秒等待
        osDelay(pdMS_TO_TICKS(5000U)) != osOK)              // 实睡5秒等系统起来
        goto health_fail;
    printf("[TEL] task started\r\n");              // 打印任务启动日志

    for (;;)                                      // 任务死循环，一直跑不退出
    {                                             // 循环体开始
        taskENTER_CRITICAL();                      // 进临界区，保证标志读写不被打断
        bool healthy = Monitor_ReportProgress(APP_MODULE_TELEMETRY, true);  // 喂狗，报告我还活着
        // 只要有暂停请求，或者OTA让静默，都算暂停状态
        bool paused = s_pause_requested || !UpgradeQuiesce_AllowsMutation();  // 暂停或升级静默都算暂停
        // 有暂停请求且我健康，才确认停稳，让请求方知道
        s_pause_acknowledged = s_pause_requested && healthy;  // 请求且健康才确认
        taskEXIT_CRITICAL();                       // 退出临界区
        if (!healthy) goto health_fail;            // 看门狗报错了，跳错误处理
        if (paused)                                // 处于暂停状态
        {                                           // 暂停分支开始
            // 暂停状态下每500毫秒醒一次，看看有没有恢复请求
            if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 1500U) ||  // 登记1.5秒等待
                osDelay(pdMS_TO_TICKS(500U)) != osOK)               // 实睡500毫秒
                goto health_fail;
            continue;                              // 跳过本轮采集，直接进下一轮循环
        }                                           // 暂停分支结束

        // 阶段1：收集有效从站ID，M1只持有配置锁很短时间，回调里只读ID
        tel_slave_list_t list = {0};               // 临时ID列表，先清零
        if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, 3000U)) goto health_fail;  // 喂狗
        uint8_t cnt = mb_for_each_valid_slave(collect_slave_ids, &list);  // 遍历M1有效从站，收集ID
        if (!Monitor_ReportProgress(APP_MODULE_TELEMETRY, true)) goto health_fail;  // 喂狗

        // 阶段2：逐个从站处理，此时不持有任何Modbus锁，安全
        for (uint8_t i = 0; i < list.n; i++)       // 遍历收集到的从站ID
        {                                           // 循环体开始
            // 处理到一半收到暂停请求就立刻停，不继续处理
            if (s_pause_requested || !UpgradeQuiesce_AllowsMutation()) break;  // 暂停或静默就跳出
            if (!publish_one_slave(list.ids[i])) goto health_fail;  // 处理一个从站，返回失败就是看门狗挂了
        }                                           // 循环体结束

        printf("[TEL] cycle done: %u slaves\r\n", (unsigned)cnt);  // 打印这一轮处理了多少从站
        // 等2秒再开始下一轮，留3秒余量给看门狗，防止延时期间超时
        if (!Monitor_BeginWait(APP_MODULE_TELEMETRY, TELEMETRY_PERIOD_MS + 1000U) ||  // 登记3秒等待
            osDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS)) != osOK)                    // 实睡2秒
            goto health_fail;
    }                                             // 死循环结束

health_fail:                                    // 健康故障标签，看门狗挂了就跳到这
    taskENTER_CRITICAL();                        // 进临界区改标志
    s_pause_acknowledged = false;                // 清掉暂停确认，告诉外面我没在正常停着
    taskEXIT_CRITICAL();                         // 退出临界区
    Monitor_ReportFatal(APP_MODULE_TELEMETRY);   // 向监控模块上报致命错误，任务卡死了
    osThreadExit();                              // 结束当前任务，不再跑了
}                                               // 主任务函数结束
