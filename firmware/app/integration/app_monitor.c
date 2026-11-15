#include "app_monitor.h"
#include "app_upgrade.h"
#include "upgrade_stage.h"
#include "watchdog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "main.h"

#define MONITOR_STARTUP_DEADLINE_MS 120000U

typedef enum
{
  MONITOR_STARTING,
  MONITOR_RUNNING,
  MONITOR_STARTUP_EXPIRED
} MonitorStartupState;

static bool product_healthy;
static bool product_reported;
static bool watchdog_healthy;
static bool fatal_reported;
static uint32_t startup_started_ms;
static MonitorStartupState startup_state;

typedef struct
{
  bool registered;
  bool reported;
  bool self_test_passed;
  bool waiting;
  bool fatal;
  uint32_t progress_ms;
  uint32_t max_wait_ms;
  uint32_t last_progress_ms;
  uint32_t wait_started_ms;
  uint32_t wait_ms;
} MonitorModuleHealth;

static MonitorModuleHealth health[APP_MODULE_COUNT];

// 仅对实际业务模块聚合健康；配置关闭的MQTT不参与。
static bool ModuleRequired(uint32_t module)
{
#if !GATEWAY_ENABLE_M2
  if (module == APP_MODULE_MQTT || module == APP_MODULE_NETWORK_UPGRADE)
  {
    return false;
  }
#endif
  return module != APP_MODULE_MONITOR;
}

// 检查模块的工作或等待是否超过已登记期限。
static bool ProgressExpired(const MonitorModuleHealth *entry, uint32_t now_ms)
{
  return entry->waiting
       ? (uint32_t)(now_ms - entry->wait_started_ms) >= entry->wait_ms
       : (uint32_t)(now_ms - entry->last_progress_ms) >= entry->progress_ms;
}

// 检查单个模块是否已登记、报告自检通过且未超时。
static bool SelfTestReady(const MonitorModuleHealth *entry, uint32_t now_ms)
{
  return entry->registered && entry->reported && entry->self_test_passed
      && !entry->fatal && !ProgressExpired(entry, now_ms);
}

// 检查健康报告的任务上下文和模块编号。
static bool ReportContext(AppModuleId module)
{
  return ((uint32_t)module < APP_MODULE_COUNT) && (module != APP_MODULE_MONITOR)
      && (__get_IPSR() == 0U) && (osKernelGetState() == osKernelRunning);
}

// 为模块登记进展和等待上限，并建立初始监督状态。
bool Monitor_RegisterHealth(AppModuleId module, uint32_t progress_ms, uint32_t max_wait_ms)
{
  if (!ReportContext(module) || (progress_ms == 0U) || (progress_ms > 0x7FFFFFFFU)
      || (max_wait_ms == 0U) || (max_wait_ms > 0x7FFFFFFFU))
  {
    return false;
  }
  taskENTER_CRITICAL();
  MonitorModuleHealth *entry = &health[module];
  bool accepted = !entry->registered;
  if (accepted)
  {
    entry->progress_ms = progress_ms;
    entry->max_wait_ms = max_wait_ms;
    entry->last_progress_ms = HAL_GetTick();
    entry->registered = true;
  }
  taskEXIT_CRITICAL();
  return accepted;
}

// 记录模块实际进展和自检结果，拒绝逾期补报。
bool Monitor_ReportProgress(AppModuleId module, bool self_test_passed)
{
  if (!ReportContext(module))
  {
    return false;
  }
  taskENTER_CRITICAL();
  MonitorModuleHealth *entry = &health[module];
  bool accepted = entry->registered && !entry->fatal;
  if (accepted)
  {
    uint32_t now_ms = HAL_GetTick();
    bool expired = ProgressExpired(entry, now_ms);
    entry->fatal = expired; // 逾期后的迟到进展不能恢复本次喂狗资格。
    fatal_reported = fatal_reported || expired;
    accepted = !expired;
    if (accepted)
    {
      entry->last_progress_ms = now_ms;
      entry->reported = true;
      entry->self_test_passed = self_test_passed;
      entry->waiting = false;
    }
  }
  taskEXIT_CRITICAL();
  return accepted;
}

// 登记一次有界等待，禁止靠重复等待声明续期。
bool Monitor_BeginWait(AppModuleId module, uint32_t timeout_ms)
{
  if (!ReportContext(module) || (timeout_ms == 0U))
  {
    return false;
  }
  taskENTER_CRITICAL();
  MonitorModuleHealth *entry = &health[module];
  uint32_t now_ms = HAL_GetTick();
  bool accepted = entry->registered && !entry->waiting && !entry->fatal
               && (timeout_ms <= entry->max_wait_ms)
               && ((uint32_t)(now_ms - entry->last_progress_ms) < entry->progress_ms);
  if (accepted)
  {
    entry->waiting = true;
    entry->wait_started_ms = now_ms;
    entry->wait_ms = timeout_ms;
  }
  taskEXIT_CRITICAL();
  return accepted;
}

// 锁存模块不可恢复故障，禁止后续恢复喂狗。
void Monitor_ReportFatal(AppModuleId module)
{
  if (ReportContext(module))
  {
    taskENTER_CRITICAL();
    health[module].fatal = true;
    fatal_reported = true;
    taskEXIT_CRITICAL();
  }
}

// 区分TF与网络升级的实际执行任务，不从其他上下文代报进展。
static AppModuleId UpgradeModule(void)
{
  if (App_IsCurrentModuleTask(APP_MODULE_UPGRADE)) return APP_MODULE_UPGRADE;
  if (App_IsCurrentModuleTask(APP_MODULE_NETWORK_UPGRADE)) return APP_MODULE_NETWORK_UPGRADE;
  return APP_MODULE_COUNT;
}

// 升级检查点只更新当前执行任务，其他任务沿用各自的监督。
bool Monitor_UpgradeProgress(void)
{
  AppModuleId module = UpgradeModule();
  return module == APP_MODULE_COUNT || Monitor_ReportProgress(module, true);
}

// 升级任务声明当前同步操作的有界等待，不从其他任务代报其心跳。
bool Monitor_UpgradeWait(uint32_t timeout_ms)
{
  AppModuleId module = UpgradeModule();
  if (module == APP_MODULE_COUNT)
  {
    return true;
  }
  return (timeout_ms > 0U) && (timeout_ms <= UPGRADE_STAGE_PREPARE_TIMEOUT_MS)
      && Monitor_BeginWait(module, timeout_ms + 1000U);
}

// 检查各模块进展和栈余量，输出平台及产品就绪结果。
static bool CheckModuleHealth(bool startup_complete, bool *product_ready)
{
  bool runtime_healthy = true;
  bool ready = true;
  for (uint32_t i = 0U; i < APP_MODULE_COUNT; i++)
  {
    if (!ModuleRequired(i))
    {
      continue;
    }
    taskENTER_CRITICAL();
    MonitorModuleHealth snapshot = health[i];
    // 在同一临界区取时间，避免新进展与旧时间相减而被误判超时。
    uint32_t now_ms = HAL_GetTick();
    taskEXIT_CRITICAL();
    if (!snapshot.registered)
    {
      ready = false;
      continue;
    }
    if (!startup_complete && (App_GetModuleTaskState((AppModuleId)i) != APP_MODULE_TASK_STARTED))
    {
      runtime_healthy = runtime_healthy && !snapshot.fatal;
      ready = false;
      continue;
    }
    bool expired = ProgressExpired(&snapshot, now_ms);
    uint32_t free_bytes = 0U;
    bool stack_ok = App_GetModuleStackFree((AppModuleId)i, &free_bytes) && (free_bytes >= 256U);
    runtime_healthy = runtime_healthy && !snapshot.fatal && !expired && stack_ok;
    ready = ready && SelfTestReady(&snapshot, now_ms);
  }
  *product_ready = ready;
  return runtime_healthy;
}

// 初始化平台监督状态，产品自检默认未通过。
AppModuleInitResult Monitor_Init(void)
{
  product_healthy = false;
  product_reported = false;
  watchdog_healthy = true;
  fatal_reported = false;
  startup_started_ms = osKernelGetTickCount();
  startup_state = MONITOR_STARTING;
  return APP_MODULE_INIT_OK;
}

// 记录产品自检结果，供最终试运行确认使用。
void Monitor_ReportProductHealth(bool healthy)
{
  if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning))
  {
    return;
  }
  taskENTER_CRITICAL();
  product_reported = true;
  product_healthy = healthy;
  taskEXIT_CRITICAL();
}

// 记录装配完成状态，不延长启动监督期限。
void Monitor_StartupComplete(void)
{
  taskENTER_CRITICAL();
  if (startup_state == MONITOR_STARTING)
  {
    startup_state = ((uint32_t)(osKernelGetTickCount() - startup_started_ms)
                      < MONITOR_STARTUP_DEADLINE_MS)
                  ? MONITOR_RUNNING : MONITOR_STARTUP_EXPIRED;
  }
  taskEXIT_CRITICAL();
}

// 启动期允许业务任务尚未创建，但装配错误始终算故障；不判断网络在线或TF存在。
static bool PlatformHealthy(uint32_t previous_hal_ms, bool startup_complete)
{
  if ((osKernelGetState() != osKernelRunning) || (HAL_GetTick() == previous_hal_ms)
      || (xPortGetMinimumEverFreeHeapSize() < 256U)
      || (uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t) < 256U))
  {
    return false;
  }
  for (uint32_t i = 0U; i < APP_MODULE_COUNT; i++)
  {
    AppModuleTaskState state = App_GetModuleTaskState((AppModuleId)i);
    if ((state == APP_MODULE_TASK_NOT_CONNECTED) || (state == APP_MODULE_TASK_STARTED))
    {
      continue;
    }
    if (!startup_complete && (state == APP_MODULE_TASK_NOT_STARTED))
    {
      continue;
    }
    return false;
  }
  return true;
}

// 复查健康条件后写入确认邮箱并复位；条件不满足时不写确认消息。
static void ResetForUpgrade(AppUpgradeAction action)
{
  // 最后确认与健康报告原子衔接：故障不能在Poll后、写邮箱前被旧快照漏掉。
  taskENTER_CRITICAL();
  bool can_confirm = watchdog_healthy && !fatal_reported && (!product_reported || product_healthy);
  uint32_t now_ms = HAL_GetTick();
  for (uint32_t i = 0U; i < APP_MODULE_COUNT; i++)
  {
    if (!ModuleRequired(i))
    {
      continue;
    }
    can_confirm = can_confirm && SelfTestReady(&health[i], now_ms);
  }
  if ((action != APP_UPGRADE_CONFIRM_RESET) || !can_confirm
      || !App_UpgradeWriteConfirmation((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS))
  {
    UpgradeMailbox_Clear((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS);
  }
  // 此函数不返回；保持不可抢占直到复位，避免确认邮箱写完后再被故障报告打断。
  __disable_irq();
  __DSB();
  NVIC_SystemReset();
}

// 周期检查平台和业务健康，处理升级状态并决定是否喂狗。
void Monitor_Task(void *argument)
{
  (void)argument;
  uint32_t previous_hal_ms = HAL_GetTick();
  for (;;)
  {
    // 先等待一个观察周期，避免第一轮把相同的HAL tick误判为时基停止。
    if (osDelay(100U) != osOK)
    {
      Error_Handler();
      return;
    }
    uint32_t now_ms = osKernelGetTickCount(); // 本工程1000Hz，即1tick=1ms。
    // 处理收包超时及已提交升级的复位，不执行 Flash 或密码操作。
    UpgradeStage_Poll();
    taskENTER_CRITICAL();
    if ((startup_state == MONITOR_STARTING)
        && ((uint32_t)(now_ms - startup_started_ms) >= MONITOR_STARTUP_DEADLINE_MS))
    {
      startup_state = MONITOR_STARTUP_EXPIRED;
    }
    MonitorStartupState state = startup_state;
    bool healthy = !product_reported || product_healthy;
    taskEXIT_CRITICAL();

    if (state == MONITOR_STARTUP_EXPIRED)
    {
      ResetForUpgrade(APP_UPGRADE_ROLLBACK_RESET);
      return;
    }
    bool startup_complete = state == MONITOR_RUNNING;
    bool platform_healthy = PlatformHealthy(previous_hal_ms, startup_complete);
    bool modules_ready = false;
    platform_healthy = CheckModuleHealth(startup_complete, &modules_ready) && platform_healthy;
    healthy = healthy && modules_ready;
    previous_hal_ms = HAL_GetTick();
    // 平台故障一旦观察到就停止喂狗，也禁止随后把本次试运行确认成功。
    taskENTER_CRITICAL();
    watchdog_healthy = watchdog_healthy && platform_healthy && !fatal_reported;
    taskEXIT_CRITICAL();
    AppUpgradeAction action = App_UpgradePoll(now_ms,
                                              watchdog_healthy && startup_complete, healthy);
    if (action != APP_UPGRADE_WAIT)
    {
      ResetForUpgrade(action);
      return;
    }
    taskENTER_CRITICAL();
    if (watchdog_healthy && !fatal_reported)
    {
      Watchdog_Refresh();
    }
    taskEXIT_CRITICAL();
  }
}
