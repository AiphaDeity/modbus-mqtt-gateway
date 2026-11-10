#ifndef APP_MODULES_H
#define APP_MODULES_H

/*
 * 装配层在调度器启动后调用 init，准备模块资源，不在 init 中创建业务任务或无限等待。
 * NOT_READY 表示可创建任务但业务条件未就绪；无法安全创建任务时返回 FAILED。
 * 业务任务统一接收 NULL 参数，运行期间不得返回；执行前检查配置和资源。
 * 耗时恢复、I/O 和重连由所属任务处理；未就绪时有界等待或限频重试。
 * ETH 接收和 LwIP tcpip 线程由网络适配创建，装配层不重复创建；
 * 对应优先级为 osPriorityHigh（40）和 osPriorityAboveNormal（32）。
 */

#include <stdbool.h>
#include <stdint.h>

// 基础初始化结果；OK 不代表网络在线、TF 可用或数据已持久化。
typedef enum
{
  APP_MODULE_INIT_OK = 0,
  APP_MODULE_INIT_NOT_READY,
  APP_MODULE_INIT_FAILED
} AppModuleInitResult;

// 模块编号，同时确定基础初始化顺序
typedef enum
{
  APP_MODULE_STORAGE = 0,
  APP_MODULE_CONFIG,
  APP_MODULE_MODBUS,
  APP_MODULE_MQTT,
  APP_MODULE_TELEMETRY,
  APP_MODULE_MONITOR,
  APP_MODULE_COUNT
} AppModuleId;

// STARTED 只表示任务创建成功，不表示业务或外设就绪
typedef enum
{
  APP_MODULE_TASK_NOT_CONNECTED = 0,
  APP_MODULE_TASK_NOT_STARTED,
  APP_MODULE_TASK_STARTED,
  APP_MODULE_TASK_BINDING_ERROR,
  APP_MODULE_TASK_INIT_FAILED,
  APP_MODULE_TASK_CREATE_FAILED,
  APP_MODULE_TASK_RETURNED,
  APP_MODULE_TASK_INVALID
} AppModuleTaskState;

// App_Entry 在其他模块初始化前启动监督任务，使用静态 TCB 和栈。
void App_MonitorStart(void);

// 仅由 App_Entry 在监督任务启动后调用一次
void App_ModulesInit(void);

// 初始化完成后集中创建已接入任务；重复调用不会重复创建
void App_ModulesStart(void);

// 查询基础初始化结果；不表示模块当前业务状态
AppModuleInitResult App_GetModuleInitResult(AppModuleId module);

// 查询单个模块的装配状态；非法模块编号返回 TASK_INVALID
AppModuleTaskState App_GetModuleTaskState(AppModuleId module);


// 存储服务初始化与任务入口。
AppModuleInitResult Storage_Init(void);
void Storage_Task(void *argument);

// 配置维护初始化与任务入口。
AppModuleInitResult Config_Init(void);
void Config_Task(void *argument);

// Modbus_Init 无返回值，由装配适配成模块初始化结果。
void Modbus_Init(void);
void vModbusPollTask(void *argument);

// MQTT 服务初始化与任务入口。
AppModuleInitResult Mqtt_Init(void);
void Mqtt_Task(void *argument);

// 遥测上报模块：M2（周期从 M1 拿数据，拼 JSON 投 M3 写队列）
AppModuleInitResult Telemetry_Init(void);
void Telemetry_Task(void *argument);

// 运行监督初始化与任务入口。
AppModuleInitResult Monitor_Init(void);
void Monitor_Task(void *argument);

// 只读查询已启动模块的历史最小栈余量（B）；无有效任务返回false。
bool App_GetModuleStackFree(AppModuleId module, uint32_t *bytes);
// 查询当前任务是否为指定模块已登记的任务。
bool App_IsCurrentModuleTask(AppModuleId module);

#endif
