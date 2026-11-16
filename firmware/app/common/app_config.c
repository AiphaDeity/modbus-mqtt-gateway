/*
 * app_config.c —— 应用全局配置实现
 *
 * 功能：
 *   1. 提供默认配置（上电时 Flash 里没存配置就用这个）
 *   2. 从 Flash 加载配置（M3 Storage 模块提供）
 *   3. 保存配置到 Flash
 *
 * 设计：
 *   - 默认值集中在这里，改参数只改这一处
 *   - 配置存在 SPI Flash（W25Q128）里，掉电不丢
 *   - 上电先读 Flash，CRC 校验不对就用默认值
 */
#include "app_config.h"              // 本文件对应的头文件，里面有 app_config_t 结构体定义
#include "upgrade_quiesce.h"        // OTA 升级暂停模块，升级时禁止改配置
#include "FreeRTOS.h"               // FreeRTOS 总头文件
#include "task.h"                   // FreeRTOS 任务相关头文件，里面有临界区函数
#include "cmsis_os2.h"              // CMSIS-RTOS2 封装层，提供 osKernelGetState 等函数
#include "stm32f4xx.h"              // STM32F4 寄存器定义，里面有 __get_IPSR 函数
#include <string.h>                 // C 标准字符串库，提供 memcpy 内存拷贝函数

// 默认配置：第一次上电、或者 Flash 里配置损坏时用这个
// static 表示这个变量只在本文件里能看到，其他文件访问不到
// const 表示这个变量是只读的，程序运行时不能改，防止不小心把默认值改坏了
static const app_config_t s_default_config = {   // 默认配置结构体定义，用指定初始化器逐字段赋值
    // 公共头：用来校验配置是不是合法的
    .magic = CONFIG_MAGIC,                    // 魔数：固定的标识，用来判断这是不是我们的配置
    .version = CONFIG_VERSION,                // 配置版本号：以后配置结构改了，靠版本号区分
    .crc32 = 0,                               // CRC32 校验值：这里先写0，保存到Flash时由Storage模块算

    // Modbus 参数（M1 模块负责的串口通信参数）
    .mb_slave_count = 4,                      // 总共有多少个 Modbus 从机（传感器）
    .mb_slave_addr = {1, 2, 3, 4},            // 每个从机的地址，从1到4，对应4个传感器
    .mb_baudrate = 9600,                      // 串口波特率：每秒传9600个比特，匹配传感器出厂默认
    .mb_data_bits = 8,                        // 数据位：每个字节用8个比特表示
    .mb_stop_bits = 1,                        // 停止位：每个字节传完后用1个比特表示结束
    .mb_parity = 0,                           // 校验位：0表示无校验（NONE）
    .mb_flow_ctrl = 0,                        // 流控：0表示无流控（NONE）
    .mb_poll_interval_ms = 1000,              // 轮询间隔：每隔1000毫秒（1秒）问一次传感器

    // MQTT 参数（M2 模块负责的上云参数）
    .mq_broker_host = "192.168.2.45",         // MQTT 服务器地址：本地测试用的 mosquitto 地址
    .mq_broker_port = 1883,                   // MQTT 服务器端口：默认1883端口
    .mq_client_id = "mb2mqtt_001",            // 客户端ID：每个设备的唯一标识，服务器靠这个区分设备
    .mq_username = "",                        // 用户名：本地测试不用认证，所以为空
    .mq_password = "",                        // 密码：本地测试不用认证，所以为空
    .mq_pub_topic = "mb2mqtt_001/telemetry",  // 发布主题：遥测数据发到这个主题
    .mq_sub_topic = "mb2mqtt_001/cmd",        // 订阅主题：从这个主题收服务器下发的命令
    .mq_report_interval_ms = 5000,             // 上报间隔：每隔5000毫秒（5秒）发一次遥测数据
};

// 全局配置实例：上电后从 Flash 加载，或者用默认值
// static 表示只在本文件里能看到，其他文件要通过 AppConfig_Get 等函数访问
static app_config_t s_config;                // 全局配置实例，运行时真正生效的那份

// 函数：AppConfig_GetDefault —— 获取默认配置（对外接口）
// 功能：上电时Flash里没有有效配置（首次开机或配置区损坏）时用这个函数拿默认配置：本地mosquitto的broker地址、默认端口1883、默认客户端ID等，保证设备没配置也能以出厂参数连本地环境
// 参数：cfg —— 输出参数，指向调用方提供的app_config_t结构体，函数把整套默认值按字段拷进去
// 返回：void，无返回值，纯内存拷贝不会失败
void AppConfig_GetDefault(app_config_t *cfg)  // 对外接口：把默认配置拷给调用方
{
    // 把默认配置结构体整个拷贝到 cfg 指向的内存
    // 第一个参数：目标地址（要拷到哪里）
    // 第二个参数：源地址（从哪里拷）
    // 第三个参数：拷贝多少字节，sizeof(app_config_t) 就是整个配置结构体的大小
    memcpy(cfg, &s_default_config, sizeof(app_config_t));
}

// 函数：AppConfig_Get —— 获取当前配置（对外接口）
// 功能：别的模块（MQTT、遥测等）要读运行配置时调用，返回全局配置结构体的只读指针，避免每个模块自己存一份配置副本
// 参数：无
// 返回：const app_config_t*，指向全局配置的只读指针，只能读不能改，防止调用方误改全局配置破坏一致性
const app_config_t *AppConfig_Get(void)       // 对外接口：返回当前配置只读指针
{
    return &s_config;                             // 返回全局配置实例的地址
}

// 函数：AppConfig_Set —— 更新当前配置（对外接口）
// 功能：收到云端下发的配置修改命令后调用：校验参数非空，把新配置整体拷贝到全局配置结构体，RAM里立即生效；OTA升级静默期拒绝修改防止升级中途配置变动
// 参数：cfg —— 指向新配置的指针，内容会被拷贝进全局配置
// 返回：bool，true代表RAM里的配置已更新成功；false代表参数为空或升级静默期不允许改
bool AppConfig_Set(const app_config_t *cfg)   // 对外接口：更新RAM里的当前配置
{
    // 先检查三个条件，有一个不满足就直接返回 false
    // 1. cfg == NULL：传进来的指针是空的，没法拷贝
    // 2. __get_IPSR() != 0U：当前在中断里，不能在中断里做这种耗时操作
    // 3. osKernelGetState() != osKernelRunning：操作系统还没跑起来，不能用临界区
    if (cfg == NULL || __get_IPSR() != 0U || osKernelGetState() != osKernelRunning)  // 空指针/在中断里/内核没跑，三个任一满足拒绝
        return false;

    taskENTER_CRITICAL();                        // 进入临界区：关中断，防止拷贝到一半被打断
    bool allowed = UpgradeQuiesce_AllowsMutation();  // 检查OTA升级时允不允许改配置
    if (allowed)                                 // 允许改才拷贝
        memcpy(&s_config, cfg, sizeof(app_config_t));  // 把新配置整个拷贝到全局变量
    taskEXIT_CRITICAL();                         // 退出临界区：开中断，恢复正常
    return allowed;                              // 返回是否允许修改
}
