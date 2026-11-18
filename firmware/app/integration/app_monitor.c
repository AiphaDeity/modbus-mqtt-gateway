#include "app_monitor.h"
#include "app_upgrade.h"
#include "upgrade_stage.h"
#include "watchdog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "main.h"

// 从监督初始化开始累计的装配总期限，单位 ms，不是单次业务操作的超时。
#define MONITOR_STARTUP_DEADLINE_MS 120000U

typedef enum
{
  MONITOR_STARTING,
  MONITOR_RUNNING,
  MONITOR_STARTUP_EXPIRED
} MonitorStartupState;

static bool product_healthy;
static bool product_reported;
static bool watchdog_healthy; // 保留此前监督结论；循环中一旦为 false，本次运行不再恢复。
static bool fatal_reported; // 模块主动报告或迟到进展触发的全局致命故障锁存。
static uint32_t startup_started_ms;
static MonitorStartupState startup_state;

typedef struct
{
  bool registered;
  bool reported;
  bool self_test_passed;
  bool waiting; // 使用 wait_started_ms/wait_ms 检查本次合法等待，否则检查工作进展。
  bool fatal;
  uint32_t progress_ms; // 两次实际进展之间允许的最大间隔，单位 ms。
  uint32_t max_wait_ms;
  uint32_t last_progress_ms; // 最近一次进展的 HAL 时刻，单位 ms。
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

// 正常工作按最近进展检查间隔；显式等待按本次等待起点和时长检查，均不能靠轮询自动续期。
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

/*
 * 模块任务启动时登记正常进展间隔和单次等待上限，单位 ms，不建任务、不直接喂狗。
 * 仅接受首次登记；登记上限不表示已开始等待，也不表示模块自检通过。
 */
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
    // 以当前时刻开始正常工作监督；模块后续须报告真实进展或显式声明一次有界等待。
    entry->progress_ms = progress_ms;
    entry->max_wait_ms = max_wait_ms;
    entry->last_progress_ms = HAL_GetTick();
    entry->registered = true;
  }
  taskEXIT_CRITICAL();
  return accepted;
}

/*
 * 模块完成实际工作后报告进展，同时更新自检结果；自检暂未通过仍可报告正常推进。
 * 已登记的工作/等待期限一旦超过就锁存故障，迟到报告不能重建本轮喂狗资格。
 */
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
      // 接受报告后更新进展时刻和自检快照，并结束先前声明的等待；这里仍不刷新硬件。
      entry->last_progress_ms = now_ms;
      entry->reported = true;
      entry->self_test_passed = self_test_passed;
      entry->waiting = false;
    }
  }
  taskEXIT_CRITICAL();
  return accepted;
}

/*
 * 耗时操作开始前声明本次等待，timeout_ms 不能超过登记的单次上限。
 * 已在等待、此前已超期或故障已锁存时拒绝；接受后只按本次起点计时，不能重复调用续期。
 */
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

/*
 * 汇总各模块运行健康并返回，同时通过 product_ready 输出是否全部必需模块自检就绪。
 * 自检未通过只阻止试运行确认；已登记模块的超期、致命故障和栈余量不足才使本检查判为不健康。
 */
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
      // 未登记不能通过产品确认，但不在这里将它直接当作已发生的运行超期。
      ready = false;
      continue;
    }
    if (!startup_complete && (App_GetModuleTaskState((AppModuleId)i) != APP_MODULE_TASK_STARTED))
    {
      runtime_healthy = runtime_healthy && !snapshot.fatal;
      ready = false;
      continue;
    }
    // 自检通过用于产品就绪；进展、致命错误和栈余量用于决定平台能否继续喂狗。
    bool expired = ProgressExpired(&snapshot, now_ms);
    uint32_t free_bytes = 0U;
    // 读取历史最低栈余量，256 B 是监督门槛，不表示已经发生栈越界。
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
  // 初始化建立本次监督起点；运行循环只会保留或撤销喂狗资格，不会将失败重新设为 true。
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

/*
 * App_Entry 在批量建任务后通知装配结束，Monitor 随后按运行期规则检查模块状态。
 * 不修改资源就绪位或自检结果，也不是业务任务开始执行的开关。
 * 仍按原启动时刻核对期限；超期不能靠补发完成通知恢复为正常。
 */
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
  // 同时检查调度、HAL 时基、RTOS 堆及本任务栈的历史最低余量；256 B 是门槛，不是溢出判据。
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

/*
 * 执行 App_UpgradePoll 给出的复位动作：在同一临界区内复查健康，写入或清除邮箱后软件复位。
 * 即使动作是 CONFIRM，最终检查未通过也只作不带确认的复位；Boot 再决定重试或回退。
 */
static void ResetForUpgrade(AppUpgradeAction action)
{
  // 1. 最后确认与健康报告原子衔接：故障不能在 Poll 后、写邮箱前被旧快照漏掉。
  taskENTER_CRITICAL();
  bool can_confirm = watchdog_healthy && !fatal_reported && (!product_reported || product_healthy);
  uint32_t now_ms = HAL_GetTick();
  // 逐项复查必需模块已报告的自检结果及进展/等待期限，不只依赖上一轮汇总值。
  for (uint32_t i = 0U; i < APP_MODULE_COUNT; i++)
  {
    if (!ModuleRequired(i))
    {
      continue;
    }
    can_confirm = can_confirm && SelfTestReady(&health[i], now_ms);
  }
  // 2. 只有确认动作且当前全部条件仍满足，才保留确认消息；其余路径明确清除旧有效标记。
  if ((action != APP_UPGRADE_CONFIRM_RESET) || !can_confirm
      || !App_UpgradeWriteConfirmation((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS))
  {
    UpgradeMailbox_Clear((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS);
  }
  // 3. 此函数不返回；保持不可抢占直到复位，避免确认邮箱写完后再被故障报告打断。
  // 软件复位重新进入 Boot 的 Reset_Handler，不在本任务里直接调用安装器。
  __disable_irq();
  __DSB();
  NVIC_SystemReset();
}

/*
 * 由 App_MonitorStart 提前创建并经 App_ModuleTaskEntry 调用，持续监督装配与运行。
 * 每轮先看进展及资源余量，再让 App_UpgradePoll 判断试运行动作，最后决定是否刷新 IWDG。
 * 检查放在任务中执行；业务任务或 Monitor 自身无法被调度时，不会由滴答中断替它继续喂狗。
 */
void Monitor_Task(void *argument)
{
  (void)argument;
  uint32_t previous_hal_ms = HAL_GetTick();
  for (;;)
  {
    // 1. 每轮先延迟 100 tick，本工程约为 100 ms，再加本轮执行和调度耗时，不是精确周期。
    // 等待让业务任务运行，也避免第一轮把相同 HAL tick 当作时基停止；它不是 IWDG 的超时时长。
    if (osDelay(100U) != osOK)
    {
      Error_Handler();
      return;
    }
    uint32_t now_ms = osKernelGetTickCount(); // 本工程1000Hz，即1tick=1ms。
    // 2. 推进接收超时或已持久 READY 的复位；实际 Flash 和密码工作留在升级/存储任务。
    UpgradeStage_Poll();
    taskENTER_CRITICAL();
    if ((startup_state == MONITOR_STARTING)
        && ((uint32_t)(now_ms - startup_started_ms) >= MONITOR_STARTUP_DEADLINE_MS))
    {
      startup_state = MONITOR_STARTUP_EXPIRED;
    }
    MonitorStartupState state = startup_state;
    // 未提供额外产品报告时，最终产品就绪仍须由各模块的真实自检共同满足。
    bool healthy = !product_reported || product_healthy;
    taskEXIT_CRITICAL();

    // 3. 装配总期限已到仍未完成，就不再等待，按不带健康确认的方式复位交给 Boot。
    if (state == MONITOR_STARTUP_EXPIRED)
    {
      ResetForUpgrade(APP_UPGRADE_ROLLBACK_RESET);
      return;
    }
    // 4. 汇总平台状态、模块真实进展、自检和堆栈余量；任务已创建不能代替这些检查。
    bool startup_complete = state == MONITOR_RUNNING;
    bool platform_healthy = PlatformHealthy(previous_hal_ms, startup_complete);
    bool modules_ready = false;
    platform_healthy = CheckModuleHealth(startup_complete, &modules_ready) && platform_healthy;
    healthy = healthy && modules_ready;
    previous_hal_ms = HAL_GetTick();
    // 5. 上一轮资格与本轮检查相与：平台故障一旦观察到就锁存，后续健康报告不能改回 true。
    taskENTER_CRITICAL();
    watchdog_healthy = watchdog_healthy && platform_healthy && !fatal_reported;
    taskEXIT_CRITICAL();
    // 6. 把本轮健康结果交给试运行状态机；它只返回等待、确认复位或不带确认的复位动作。
    AppUpgradeAction action = App_UpgradePoll(now_ms,
                                              watchdog_healthy && startup_complete, healthy);
    if (action != APP_UPGRADE_WAIT)
    {
      // ResetForUpgrade 复查健康后写或清邮箱，再复位；Boot 根据邮箱及持久记录处理结果。
      ResetForUpgrade(action);
      return;
    }
    // 7. 本轮没有请求复位且监督仍健康，才刷新 IWDG；否则等待硬件超时复位。
    taskENTER_CRITICAL();
    if (watchdog_healthy && !fatal_reported)
    {
      // 业务任务只报告进展，统一由此处在监督通过后喂狗；底层 Refresh 仅执行寄存器重装。
      Watchdog_Refresh();
    }
    taskEXIT_CRITICAL();
  }
}
