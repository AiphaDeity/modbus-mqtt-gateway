#include "app_modules.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "main.h"
#include "storage.h"
#include "mqtt_client.h"
#include "telemetry_task.h"
#include "mb_api.h"
#include "upgrade_tf.h"
#if GATEWAY_ENABLE_M2
#include "network_upgrade.h"
#endif

#define STORAGE_STACK_SIZE_BYTES (6U * 1024U)  /* FatFs + storage_record，M3 说 8K 实测后可降 6K */
#define CONFIG_STACK_SIZE_BYTES  (1536U)        /* Config值拷贝RPC和有界JSON解析，仍使用原静态任务 */
#define MODBUS_STACK_SIZE_BYTES  (2U * 1024U)
#define MQTT_STACK_SIZE_BYTES    (6U * 1024U)   /* netconn + snprintf 调用链深，4K 实测溢出 */
#define TELEMETRY_STACK_SIZE_BYTES (2560U)       /* snprintf + printf 栈开销大，2.5K */
#define MONITOR_STACK_SIZE_BYTES (2U * 1024U) // 监督任务使用 2 KiB 栈。
#define UPGRADE_STACK_SIZE_BYTES (6U * 1024U)

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
 * 存储栈使用普通 SRAM，确保栈上的 FIL/缓冲可供 DMA 访问；监督栈也使用普通 SRAM。
 * 标记 CCMRAM 的其他栈不可供 DMA 访问，其调用链不得把栈缓冲直接交给 DMA。
 * 所有栈按 Cortex-M4 FreeRTOS 端口要求对齐到 8 B。
 */
_Alignas(8) static StackType_t storage_stack[STORAGE_STACK_SIZE_BYTES / sizeof(StackType_t)];
_Alignas(8) static StackType_t config_stack[CONFIG_STACK_SIZE_BYTES / sizeof(StackType_t)]  __attribute__((section(".bss.ccmram")));
_Alignas(8) static StackType_t modbus_stack[MODBUS_STACK_SIZE_BYTES / sizeof(StackType_t)]  __attribute__((section(".bss.ccmram")));
_Alignas(8) static StackType_t mqtt_stack[MQTT_STACK_SIZE_BYTES / sizeof(StackType_t)]  __attribute__((section(".bss.ccmram")));
_Alignas(8) static StackType_t telemetry_stack[TELEMETRY_STACK_SIZE_BYTES / sizeof(StackType_t)]  __attribute__((section(".bss.ccmram")));
_Alignas(8) static StackType_t monitor_stack[MONITOR_STACK_SIZE_BYTES / sizeof(StackType_t)];
_Alignas(8) static StackType_t upgrade_stack[UPGRADE_STACK_SIZE_BYTES / sizeof(StackType_t)];
#if GATEWAY_ENABLE_M2
_Alignas(8) static StackType_t network_upgrade_stack[NETWORK_UPGRADE_TASK_STACK_BYTES / sizeof(StackType_t)]
  __attribute__((section(".bss.ccmram")));
static StaticTask_t network_upgrade_control_block __attribute__((section(".bss.ccmram")));
#endif

static StaticTask_t storage_control_block;
static StaticTask_t config_control_block;
static StaticTask_t modbus_control_block;
static StaticTask_t mqtt_control_block;
static StaticTask_t telemetry_control_block;
static StaticTask_t monitor_control_block;
static StaticTask_t upgrade_control_block;

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
  .priority = osPriorityNormal // 与存储同级，启动扫描期间配置任务仍须按期报告进展。
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
  .priority = osPriorityNormal    /* ★原 BelowNormal(16) 被 MQTT/Storage(24) 饿死，改 Normal */
};

static const osThreadAttr_t monitor_attributes =
{
  .name = "monitor",
  .cb_mem = &monitor_control_block,
  .cb_size = sizeof(monitor_control_block),
  .stack_mem = monitor_stack,
  .stack_size = sizeof(monitor_stack),
  .priority = osPriorityAboveNormal // 高于启动任务；每100ms只做短检查，不执行业务I/O。
};

static const osThreadAttr_t upgrade_attributes =
{
  .name = "upgrade",
  .cb_mem = &upgrade_control_block,
  .cb_size = sizeof(upgrade_control_block),
  .stack_mem = upgrade_stack,
  .stack_size = sizeof(upgrade_stack),
  .priority = osPriorityNormal
};

#if GATEWAY_ENABLE_M2
static const osThreadAttr_t network_upgrade_attributes =
{
  .name = "nw_upgrade",
  .cb_mem = &network_upgrade_control_block,
  .cb_size = sizeof(network_upgrade_control_block),
  .stack_mem = network_upgrade_stack,
  .stack_size = sizeof(network_upgrade_stack),
  .priority = NETWORK_UPGRADE_TASK_PRIORITY
};
#endif

/*
 * init/task 必须成对绑定；均为 NULL 表示未接入，只绑定一个视为错误。
 * 监督任务提前创建；其余任务在基础初始化完成后，经统一入口创建。
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
    .init = Config_Init, .task = Config_Task, .attributes = &config_attributes,
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  },
  [APP_MODULE_MODBUS] =
  {
    .init = modbus_init_wrapper, .task = vModbusPollTask, .attributes = &modbus_attributes,
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  },
  [APP_MODULE_MQTT] =
  {
#if GATEWAY_ENABLE_M2
    .init = MqttClient_Init, .task = MqttClient_Task, .attributes = &mqtt_attributes,
#else
    .init = NULL, .task = NULL, .attributes = &mqtt_attributes,
#endif
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  },
  [APP_MODULE_TELEMETRY] =
  {
#if GATEWAY_ENABLE_M2
    .init = Telemetry_Init, .task = Telemetry_Task, .attributes = &telemetry_attributes,
#else
    .init = NULL, .task = NULL, .attributes = &telemetry_attributes,
#endif
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  },
  [APP_MODULE_MONITOR] =
  {
    .init = Monitor_Init, .task = Monitor_Task, .attributes = &monitor_attributes,
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  },
  [APP_MODULE_UPGRADE] =
  {
    .init = UpgradeTf_Init, .task = UpgradeTf_Task, .attributes = &upgrade_attributes,
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  },
  [APP_MODULE_NETWORK_UPGRADE] =
  {
#if GATEWAY_ENABLE_M2
    .init = NetworkUpgrade_Init, .task = NetworkUpgrade_Task, .attributes = &network_upgrade_attributes,
#else
    .init = NULL, .task = NULL, .attributes = NULL,
#endif
    .result = APP_MODULE_INIT_NOT_READY, .task_state = APP_MODULE_TASK_NOT_STARTED
  }
};

static AppModuleInitResult modbus_init_wrapper(void)
{
  Modbus_Init();
  return mb_runtime_ready() ? APP_MODULE_INIT_OK : APP_MODULE_INIT_FAILED;
}

static bool modules_initialized;
static bool modules_started;

/*
 * 转调模块任务，业务参数为 NULL；意外返回时记录错误并停止该任务。
 * argument 指向运行期间有效的静态模块槽。
 */
static void App_ModuleTaskEntry(void *argument)
{
  AppModuleSlot *slot = argument;

  slot->task(NULL);
  slot->task_state = APP_MODULE_TASK_RETURNED;
  Error_Handler();
}

/*
 * 在业务初始化前启动监督任务，使用装配表的静态 TCB 和栈。
 * 调度锁保证任务首次运行前槽状态完整；失败进入 Error_Handler。
 */
void App_MonitorStart(void)
{
  AppModuleSlot *slot = &module_slots[APP_MODULE_MONITOR];
  if (slot->task_state == APP_MODULE_TASK_STARTED)
  {
    return;
  }
  if (modules_initialized || modules_started || (osKernelGetState() != osKernelRunning)
      || (slot->init != Monitor_Init) || (slot->task != Monitor_Task))
  {
    Error_Handler();
    return;
  }
  if (osKernelLock() < 0)
  {
    Error_Handler();
    return;
  }
  slot->result = slot->init();
  if (slot->result != APP_MODULE_INIT_OK)
  {
    slot->task_state = APP_MODULE_TASK_INIT_FAILED;
    Error_Handler();
    return;
  }
  slot->task_handle = osThreadNew(App_ModuleTaskEntry, slot, slot->attributes);
  if (slot->task_handle == NULL)
  {
    slot->task_state = APP_MODULE_TASK_CREATE_FAILED;
    Error_Handler();
    return;
  }
  slot->task_state = APP_MODULE_TASK_STARTED;
  if (osKernelUnlock() < 0)
  {
    Error_Handler();
  }
}

/*
 * 按装配表初始化其他模块，跳过未接入和已初始化模块。
 * NOT_READY 仍允许后续创建任务，由任务等待业务条件。
 */
void App_ModulesInit(void)
{
  if (modules_initialized)
  {
    return;
  }

  if ((osKernelGetState() != osKernelRunning)
      || (module_slots[APP_MODULE_MONITOR].task_state != APP_MODULE_TASK_STARTED))
  {
    Error_Handler();
    return;
  }

  for (unsigned int i = 0; i < APP_MODULE_COUNT; ++i)
  {
    AppModuleSlot *slot = &module_slots[i];

    if (i == APP_MODULE_MONITOR)
    {
      continue; // 已提前初始化并创建，不再清空监督状态。
    }

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
 * 基础初始化后创建已绑定任务，重复调用不重复创建。
 * 创建期间锁定调度，失败保持锁定并进入 Error_Handler，避免部分任务提前运行。
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

    if ((i == APP_MODULE_MONITOR) && (state == APP_MODULE_TASK_STARTED))
    {
      continue;
    }
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

    if ((slot->task_state == APP_MODULE_TASK_NOT_CONNECTED)
        || ((i == APP_MODULE_MONITOR) && (slot->task_state == APP_MODULE_TASK_STARTED)))
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

// 读取已启动模块的历史最小空闲栈，并换算为字节。
bool App_GetModuleStackFree(AppModuleId module, uint32_t *bytes)
{
  if ((bytes == NULL) || ((uint32_t)module >= APP_MODULE_COUNT))
  {
    return false;
  }
  *bytes = 0U;
  const AppModuleSlot *slot = &module_slots[module];
  if ((slot->task_state != APP_MODULE_TASK_STARTED) || (slot->task_handle == NULL))
  {
    return false;
  }
  *bytes = (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)slot->task_handle) * sizeof(StackType_t);
  return true;
}

// 比较当前任务句柄与指定模块的已登记句柄。
bool App_IsCurrentModuleTask(AppModuleId module)
{
  return ((uint32_t)module < APP_MODULE_COUNT) && (module_slots[module].task_handle != NULL)
      && (osThreadGetId() == module_slots[module].task_handle);
}
