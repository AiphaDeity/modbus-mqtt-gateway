#ifndef APP_MODULES_H
#define APP_MODULES_H

/*
 * 模块初始化结果。
 * OK 仅表示完成模块基础初始化，
 * 不表示网络在线、TF 可用或数据已持久化。
 */
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

// 仅由 App_Entry 在启动阶段调用一次
void App_ModulesInit(void);

// 初始化完成后集中创建已接入任务；重复调用不会重复创建
void App_ModulesStart(void);

// 查询基础初始化结果；不表示模块当前业务状态
AppModuleInitResult App_GetModuleInitResult(AppModuleId module);

// 查询单个模块的装配状态；非法模块编号返回 TASK_INVALID
AppModuleTaskState App_GetModuleTaskState(AppModuleId module);

/*
 * 初始化约定：
 * 1. 由装配层在调度器启动后的任务上下文调用。
 * 2. 准备模块内部状态、私有队列、信号量和互斥锁。
 * 3. 不在初始化函数中创建业务任务。
 * 4. 不无限等待网络、TF 或其他模块。
 * 5. 耗时恢复、业务 I/O 和重连由所属任务处理。
 * 6. NOT_READY 表示基础资源已可供任务使用，但业务条件未就绪，
 *    装配层仍会创建其任务；无法安全启动任务时返回 FAILED。
 *
 * 任务约定：
 * 1. 由装配层集中创建。
 * 2. 入口参数统一为 void *，当前由装配层传入 NULL。
 * 3. 任务函数正常运行期间不得返回。
 * 4. 业务执行前检查所需配置及资源是否就绪。
 * 5. 未就绪时使用有界阻塞等待或限频重试，不忙轮询。
 * 6. ETH 接收和 LwIP tcpip 线程由 M2 按协议栈线程模型接入，
 *    不在这里重复创建；优先级分别对应 osPriorityHigh（40）和
 *    osPriorityAboveNormal（32），保持设计中的相对优先级关系。
 */

// 存储模块：M3
AppModuleInitResult Storage_Init(void);
void Storage_Task(void *argument);

// 配置维护模块：M1
AppModuleInitResult Config_Init(void);
void Config_Task(void *argument);

// Modbus 模块：M1，保留已经提供的任务名称
AppModuleInitResult Modbus_Init(void);
void vModbusPollTask(void *argument);

// MQTT 模块：M2
AppModuleInitResult Mqtt_Init(void);
void Mqtt_Task(void *argument);

// 诊断监督模块：M4
AppModuleInitResult Monitor_Init(void);
void Monitor_Task(void *argument);

#endif
