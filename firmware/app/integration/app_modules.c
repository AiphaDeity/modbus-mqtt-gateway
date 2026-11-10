#include "app_modules.h"

#include <stdbool.h>
#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "main.h"
#include "storage.h"
#include "mqtt_client.h"
#include "telemetry_task.h"

#define STORAGE_STACK_SIZE_BYTES (6U * 1024U)  /* FatFs + storage_record，M3 说 8K 实测后可降 6K */
#define CONFIG_STACK_SIZE_BYTES  (512U)         /* M1 config 还没实现，占位 */
#define MODBUS_STACK_SIZE_BYTES  (2U * 1024U)
#define MQTT_STACK_SIZE_BYTES    (6U * 1024U)   /* netconn + snprintf 调用链深，4K 实测溢出 */
#define TELEMETRY_STACK_SIZE_BYTES (2560U)       /* snprintf + printf 栈开销大，2.5K */
#define MONITOR_STACK_SIZE_BYTES (512U)         /* M4 monitor 还没实现，占位 */

typedef AppModuleInitResult (*AppModuleInitFn)(void);

/* M1 的 Modbus_Init 是 void(void)，包装成 AppModuleInitResult 返回 */
static AppModuleInitResult modbus_init_wrapper(void);

typedef struct
{
  AppModuleInitFn init;
  osThreadFunc_t task;
  const osThreadAttr_t *attributes;
  AppModuleInitResult result;
  osThreadId_t task_handle;
  // 对齐枚举的单字段诊断读写，不用于同步业务数据
  volatile AppModuleTaskState task_state;
} AppModuleSlot;

/*
 * 五个业务任务的初始栈预算共 16KiB，不含 TCB、启动任务和协议栈线程。
 * 存储、配置和 MQTT 各预留 4KiB 给库调用，Modbus 和监督各预留 2KiB。
 * 这些数值尚非实测峰值，接入后按完整调用链和栈高水位校正。
 * 五个栈、TCB 和启动任务当前均位于普通 SRAM；CCM 保留，暂不迁移任务栈。
 * DMA 缓冲仍须满足对应外设的长度、对齐和生命周期要求。
 * 栈按 Cortex-M4 FreeRTOS 端口要求对齐到 8 字节。
 */
/* 任务栈全部挪到 CCMRAM：栈是 CPU 独占访问（中断切换/函数调用），不被任何 DMA 碰。
 * 64KB CCMRAM 现在 0 占用，正好装下全部 ~15.5KB 栈，给普通 SRAM 腾地方。 */
_Alignas(8) static StackType_t storage_stack[STORAGE_STACK_SIZE_BYTES / sizeof(StackType_t)]  __attribute__((section(".ccmram")));
_Alignas(8) static StackType_t config_stack[CONFIG_STACK_SIZE_BYTES / sizeof(StackType_t)]  __attribute__((section(".ccmram")));
_Alignas(8) static StackType_t modbus_stack[MODBUS_STACK_SIZE_BYTES / sizeof(StackType_t)]  __attribute__((section(".ccmram")));
_Alignas(8) static StackType_t mqtt_stack[MQTT_STACK_SIZE_BYTES / sizeof(StackType_t)]  __attribute__((section(".ccmram")));
_Alignas(8) static StackType_t telemetry_stack[TELEMETRY_STACK_SIZE_BYTES / sizeof(StackType_t)]  __attribute__((section(".ccmram")));
_Alignas(8) static StackType_t monitor_stack[MONITOR_STACK_SIZE_BYTES / sizeof(StackType_t)]  __attribute__((section(".ccmram")));

static StaticTask_t storage_control_block;
static StaticTask_t config_control_block;
static StaticTask_t modbus_control_block;
static StaticTask_t mqtt_control_block;
static StaticTask_t telemetry_control_block;
static StaticTask_t monitor_control_block;

// CMSIS-RTOS2 的 stack_size 单位为字节；任务优先级数值越大越高
static const osThreadAttr_t storage_attributes =
{
  .name = "storage",
  .cb_mem = &storage_control_block,
  .cb_size = sizeof(storage_control_block),
  .stack_mem = storage_stack,
  .stack_size = sizeof(storage_stack),
  .priority = osPriorityNormal
};

static const osThreadAttr_t config_attributes =
{
  .name = "config",
  .cb_mem = &config_control_block,
  .cb_size = sizeof(config_control_block),
  .stack_mem = config_stack,
  .stack_size = sizeof(config_stack),
  .priority = osPriorityBelowNormal
};

static const osThreadAttr_t modbus_attributes =
{
  .name = "modbus",
  .cb_mem = &modbus_control_block,
  .cb_size = sizeof(modbus_control_block),
  .stack_mem = modbus_stack,
  .stack_size = sizeof(modbus_stack),
  .priority = osPriorityAboveNormal
};

static const osThreadAttr_t mqtt_attributes =
{
  .name = "mqtt",
  .cb_mem = &mqtt_control_block,
  .cb_size = sizeof(mqtt_control_block),
  .stack_mem = mqtt_stack,
  .stack_size = sizeof(mqtt_stack),
  .priority = osPriorityNormal
};

static const osThreadAttr_t telemetry_attributes =
{
  .name = "telemetry",
  .cb_mem = &telemetry_control_block,
  .cb_size = sizeof(telemetry_control_block),
  .stack_mem = telemetry_stack,
  .stack_size = sizeof(telemetry_stack),
  .priority = osPriorityBelowNormal
};

static const osThreadAttr_t monitor_attributes =
{
  .name = "monitor",
  .cb_mem = &monitor_control_block,
  .cb_size = sizeof(monitor_control_block),
  .stack_mem = monitor_stack,
  .stack_size = sizeof(monitor_stack),
  .priority = osPriorityBelowNormal
};

/*
 * 每个模块的 init/task 同时为 NULL 表示尚未接入。
 * 源码加入模块构建后，将同一组的两个 NULL 一起替换为已声明的函数：
 * 例如 Storage_Init、Storage_Task；只填其中一个会报告绑定错误。
 * 所有初始化先完成，再统一创建任务，任务入口由包装函数调用。
 */
static AppModuleSlot module_slots[APP_MODULE_COUNT] =
{
  [APP_MODULE_STORAGE] =
  {
    .init = Storage_Init, .task = Storage_Task, .attributes = &storage_attributes,
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  },
  [APP_MODULE_CONFIG] =
  {
    .init = NULL, .task = NULL, .attributes = &config_attributes,
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  },
  [APP_MODULE_MODBUS] =
  {
    .init = modbus_init_wrapper, .task = vModbusPollTask, .attributes = &modbus_attributes,
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  },
  [APP_MODULE_MQTT] =
  {
    .init = MqttClient_Init, .task = MqttClient_Task, .attributes = &mqtt_attributes,
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  },
  [APP_MODULE_TELEMETRY] =
  {
    .init = Telemetry_Init, .task = Telemetry_Task, .attributes = &telemetry_attributes,
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  },
  [APP_MODULE_MONITOR] =
  {
    .init = NULL, .task = NULL, .attributes = &monitor_attributes,
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  }
};

static AppModuleInitResult modbus_init_wrapper(void)
{
  Modbus_Init();
  return APP_MODULE_INIT_OK;
}

static bool modules_initialized;
static bool modules_started;

/*
 * RTOS 创建任务时使用的统一入口，argument 指向生命周期覆盖运行期的静态模块槽。
 * 转调该模块的业务任务，业务参数当前为 NULL；意外返回时记录状态并停止运行。
 */
static void App_ModuleTaskEntry(void *argument)
{
  AppModuleSlot *slot = argument;

  slot->task(NULL);
  slot->task_state = APP_MODULE_TASK_RETURNED;
  Error_Handler();
}

/*
 * 由 App_Entry 在调度器启动后调用，按装配表顺序准备各模块的基础资源。
 * 未接入模块跳过，初始化结果保存在模块槽中；重复调用不重新初始化。
 * NOT_READY 允许后续任务处理离线恢复，FAILED 留待启动前的统一检查处理。
 */
void App_ModulesInit(void)
{
  if (modules_initialized)
  {
    return;
  }

  if (osKernelGetState() != osKernelRunning)
  {
    Error_Handler();
    return;
  }

  for (unsigned int i = 0; i < APP_MODULE_COUNT; ++i)
  {
    AppModuleSlot *slot = &module_slots[i];

    if ((slot->init == NULL) && (slot->task == NULL))
    {
      slot->result = APP_MODULE_INIT_NOT_READY;
      slot->task_state = APP_MODULE_TASK_NOT_CONNECTED;
      continue;
    }

    if ((slot->init == NULL) || (slot->task == NULL))
    {
      slot->result = APP_MODULE_INIT_FAILED;
      slot->task_state = APP_MODULE_TASK_BINDING_ERROR;
      continue;
    }

    AppModuleInitResult result = slot->init();

    printf("[MOD] module[%u] init=%d state=%d\r\n", i, (int)result, (int)slot->task_state);

    switch (result)
    {
      case APP_MODULE_INIT_OK:
      case APP_MODULE_INIT_NOT_READY:
        slot->result = result;
        slot->task_state = APP_MODULE_TASK_NOT_STARTED;
        break;

      case APP_MODULE_INIT_FAILED:
      default:
        slot->result = APP_MODULE_INIT_FAILED;
        slot->task_state = APP_MODULE_TASK_INIT_FAILED;
        break;
    }
  }

  modules_initialized = true;
}

/*
 * 由 App_Entry 在全部基础初始化结束后调用，使用静态 TCB 和栈创建已接入任务。
 * 创建期间暂停任务调度，全部创建成功后恢复，重复调用不重复创建。
 * 绑定、基础初始化、任务创建或调度操作失败时进入 Error_Handler；
 * 创建中途失败时不恢复调度，确保已创建的业务任务不会单独抢先运行。
 */
void App_ModulesStart(void)
{
  if (modules_started)
  {
    return;
  }

  if (!modules_initialized || (osKernelGetState() != osKernelRunning))
  {
    Error_Handler();
    return;
  }

  // 绑定或基础初始化失败时，在创建任何业务任务前停止装配
  for (unsigned int i = 0; i < APP_MODULE_COUNT; ++i)
  {
    AppModuleTaskState state = module_slots[i].task_state;

    if ((state != APP_MODULE_TASK_NOT_CONNECTED) &&
        (state != APP_MODULE_TASK_NOT_STARTED))
    {
      Error_Handler();
      return;
    }
  }

  // 所有任务创建完成前不让高优先级任务抢先运行；中断仍然可用
  if (osKernelLock() < 0)
  {
    Error_Handler();
    return;
  }

  for (unsigned int i = 0; i < APP_MODULE_COUNT; ++i)
  {
    AppModuleSlot *slot = &module_slots[i];

    if (slot->task_state == APP_MODULE_TASK_NOT_CONNECTED)
    {
      continue;
    }

    slot->task_handle = osThreadNew(App_ModuleTaskEntry, slot, slot->attributes);
    if (slot->task_handle == NULL)
    {
      slot->task_state = APP_MODULE_TASK_CREATE_FAILED;
      // 保持调度锁并进入错误处理，避免只运行一部分业务任务
      Error_Handler();
      return;
    }

    slot->task_state = APP_MODULE_TASK_STARTED;
  }

  modules_started = true;
  if (osKernelUnlock() < 0)
  {
    Error_Handler();
  }
}

// 查询模块基础初始化结果；非法编号返回 FAILED，不代表运行期业务状态
AppModuleInitResult App_GetModuleInitResult(AppModuleId module)
{
  if ((unsigned int)module >= APP_MODULE_COUNT)
  {
    return APP_MODULE_INIT_FAILED;
  }

  return module_slots[module].result;
}

// 查询模块的单字段装配状态；非法编号返回 INVALID，不代表外设当前可用性
AppModuleTaskState App_GetModuleTaskState(AppModuleId module)
{
  if ((unsigned int)module >= APP_MODULE_COUNT)
  {
    return APP_MODULE_TASK_INVALID;
  }

  return module_slots[module].task_state;
}
