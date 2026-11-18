#include "upgrade_quiesce.h"

#include "app_upgrade.h"
#include "app_monitor.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

// 回调按模块编号保存，实际协调用 pause_order，不按枚举声明顺序遍历。
static UpgradeQuiesceParticipant participants[UPGRADE_QUIESCE_COUNT];
static uint32_t bound_mask; // 已登记完整回调的模块集合。
static uint32_t paused_mask; // 已尝试暂停的模块集合，含回调失败或尚未确认停稳的模块。
static bool quiescing;
// 每轮先处理业务生产者，最后处理存储消费者；恢复反序，先使存储可用再开放上游。
static const UpgradeQuiesceModule pause_order[] =
{
  UPGRADE_QUIESCE_NETWORK, UPGRADE_QUIESCE_CONFIG,
  UPGRADE_QUIESCE_MODBUS, UPGRADE_QUIESCE_STORAGE
};

// 根据构建配置生成必须参与升级暂停的模块集合。
static uint32_t RequiredMask(void)
{
  uint32_t mask = (1U << UPGRADE_QUIESCE_MODBUS) | (1U << UPGRADE_QUIESCE_STORAGE)
                | (1U << UPGRADE_QUIESCE_CONFIG);
#if GATEWAY_ENABLE_M2
  mask |= 1U << UPGRADE_QUIESCE_NETWORK;
#endif
  return mask;
}

// 计算共享总期限内剩余的毫秒数，到期返回零。
static uint32_t Remaining(uint32_t started_ms, uint32_t timeout_ms)
{
  uint32_t elapsed = (uint32_t)(HAL_GetTick() - started_ms);
  return elapsed < timeout_ms ? timeout_ms - elapsed : 0U;
}

// 模块初始化时登记四个入口并复制回调表；绑定不创建新任务，context 仍由模块保证有效。
UpgradeStageResult UpgradeQuiesce_Bind(UpgradeQuiesceModule module,
                                       const UpgradeQuiesceParticipant *participant)
{
  if (((uint32_t)module >= UPGRADE_QUIESCE_COUNT) || (participant == NULL)
      || (participant->pause == NULL) || (participant->settle == NULL)
      || (participant->sync == NULL) || (participant->resume == NULL))
  {
    return UPGRADE_STAGE_INVALID;
  }
  if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning))
  {
    return UPGRADE_STAGE_MODE;
  }
  taskENTER_CRITICAL();
  if (quiescing || ((bound_mask & (1U << module)) != 0U))
  {
    taskEXIT_CRITICAL();
    return UPGRADE_STAGE_BUSY;
  }
  participants[module] = *participant;
  bound_mask |= 1U << module;
  taskEXIT_CRITICAL();
  return UPGRADE_STAGE_OK;
}

// 检查所有必需模块是否均已登记回调。
bool UpgradeQuiesce_IsReady(void)
{
  taskENTER_CRITICAL();
  bool ready = (bound_mask & RequiredMask()) == RequiredMask();
  taskEXIT_CRITICAL();
  return ready;
}

/*
 * 供配置接纳、控制写入、最终发送等边界复查统一准入，避免逐个暂停模块时仍接入新变更。
 * 要求既未进入静默，又已满足 App 启动身份门控；只负责准入，不替代在途结算或资源互斥。
 */
bool UpgradeQuiesce_AllowsMutation(void)
{
  taskENTER_CRITICAL();
  bool paused = quiescing;
  taskEXIT_CRITICAL();
  return !paused && App_UpgradeCanCommit();
}

/*
 * 为升级任务登记本次有界回调等待，返回后报告本任务进展；不代替被等待模块报告健康。
 * timeout_ms 由总期限剩余时间传入，不给每个模块重新分配完整提交预算。
 */
static UpgradeStageResult CallParticipant(UpgradeStageResult (*action)(void *, uint32_t),
                                           void *context, uint32_t timeout_ms)
{
  if (!Monitor_UpgradeWait(timeout_ms))
  {
    return UPGRADE_STAGE_INTERNAL;
  }
  UpgradeStageResult result = action(context, timeout_ms);
  return Monitor_UpgradeProgress() ? result : UPGRADE_STAGE_INTERNAL;
}

/*
 * UpgradeStage_Commit 在 VERIFIED 后调用：统一关门，再分三轮请求各模块暂停、结算和同步。
 * 全部成功后返回 Stage，由 backend.commit 提交 READY；任一失败立即返回，不自行恢复业务。
 */
UpgradeStageResult UpgradeQuiesce_Run(uint32_t started_ms, uint32_t timeout_ms)
{
  if (!UpgradeQuiesce_IsReady())
  {
    return UPGRADE_STAGE_NOT_READY;
  }
  // 1. 先关闭统一变更入口，防止尚未轮到 pause 的模块在等待间隙继续接纳新控制。
  taskENTER_CRITICAL();
  quiescing = true;
  taskEXIT_CRITICAL();
  // 2. 外层控制三轮：先全部 pause，再全部 settle，最后全部 sync；每轮均按 pause_order 执行。
  // 不能逐个模块做完三步，否则下游同步时，上游可能尚在生成新的业务请求。
  for (uint32_t phase = 0U; phase < 3U; phase++)
  {
    for (uint32_t step = 0U; step < UPGRADE_QUIESCE_COUNT; step++)
    {
      uint32_t i = pause_order[step];
      if ((RequiredMask() & (1U << i)) == 0U)
      {
        continue;
      }
      // 每次只下发剩余毫秒数，回调返回后也复查同一起点，三轮和后续 READY 提交不能各自续期。
      uint32_t remaining = Remaining(started_ms, timeout_ms);
      if (remaining == 0U)
      {
        return UPGRADE_STAGE_TIMEOUT;
      }
      UpgradeQuiesceParticipant *participant = &participants[i];
      UpgradeStageResult (*action)(void *, uint32_t) = participant->pause;
      if (phase == 1U)
      {
        action = participant->settle;
      }
      else if (phase == 2U)
      {
        action = participant->sync;
      }
      if (phase == 0U)
      {
        // 调用 pause 前就登记：回调返回失败也可能已改变局部状态，恢复时不能遗漏这一模块。
        paused_mask |= 1U << i;
      }
      UpgradeStageResult result = CallParticipant(action, participant->context, remaining);
      if (Remaining(started_ms, timeout_ms) == 0U)
      {
        return UPGRADE_STAGE_TIMEOUT;
      }
      if (result != UPGRADE_STAGE_OK)
      {
        return result;
      }
    }
  }
  return UPGRADE_STAGE_OK;
}

/*
 * 提交前失败或已明确未提交时，由 Stage 逆序撤销已尝试的暂停：Storage→Modbus→Config→Network。
 * 只清除恢复成功的位；部分恢复失败仍保留统一门控及剩余集合，不能把局部恢复当作业务正常。
 */
UpgradeStageResult UpgradeQuiesce_Resume(uint32_t timeout_ms)
{
  uint32_t started_ms = HAL_GetTick();
  // 逆序恢复；任一失败保留门控，不能把部分恢复解释为正常运行。
  for (uint32_t i = UPGRADE_QUIESCE_COUNT; i > 0U; i--)
  {
    uint32_t module = pause_order[i - 1U];
    if ((paused_mask & (1U << module)) == 0U)
    {
      continue;
    }
    uint32_t remaining = Remaining(started_ms, timeout_ms);
    if (remaining == 0U)
    {
      return UPGRADE_STAGE_TIMEOUT;
    }
    UpgradeQuiesceParticipant *participant = &participants[module];
    UpgradeStageResult result = CallParticipant(participant->resume, participant->context, remaining);
    if (Remaining(started_ms, timeout_ms) == 0U)
    {
      return UPGRADE_STAGE_TIMEOUT;
    }
    if (result != UPGRADE_STAGE_OK)
    {
      return result;
    }
    paused_mask &= ~(1U << module);
  }
  // 所有曾尝试暂停的模块均已恢复，最后才重新开放统一变更入口。
  taskENTER_CRITICAL();
  quiescing = false;
  taskEXIT_CRITICAL();
  return UPGRADE_STAGE_OK;
}
