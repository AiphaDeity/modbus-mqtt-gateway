#include "upgrade_quiesce.h"

#include "app_upgrade.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

static UpgradeQuiesceParticipant participants[UPGRADE_QUIESCE_COUNT];
static uint32_t bound_mask;
static uint32_t paused_mask;
static bool quiescing;

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

// 校验并登记模块的暂停、结算、同步和恢复回调。
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

// 综合升级暂停状态和启动门控，判断是否允许新变更。
bool UpgradeQuiesce_AllowsMutation(void)
{
  taskENTER_CRITICAL();
  bool paused = quiescing;
  taskEXIT_CRITICAL();
  return !paused && App_UpgradeCanCommit();
}

// 关闭变更门控，并在同一总期限内暂停、结算和同步各模块。
UpgradeStageResult UpgradeQuiesce_Run(uint32_t started_ms, uint32_t timeout_ms)
{
  if (!UpgradeQuiesce_IsReady())
  {
    return UPGRADE_STAGE_NOT_READY;
  }
  taskENTER_CRITICAL();
  quiescing = true; // 先关门控，再请求各模块停止，避免等待间隙接入新的控制。
  taskEXIT_CRITICAL();
  for (uint32_t phase = 0U; phase < 3U; phase++)
  {
    for (uint32_t i = 0U; i < UPGRADE_QUIESCE_COUNT; i++)
    {
      if ((RequiredMask() & (1U << i)) == 0U)
      {
        continue;
      }
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
        // 失败也可能已暂停，恢复时仍须调用这一模块，不能只记成功的模块。
        paused_mask |= 1U << i;
      }
      UpgradeStageResult result = action(participant->context, remaining);
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

// 逆序恢复已暂停模块，全部成功后才重新开放变更。
UpgradeStageResult UpgradeQuiesce_Resume(uint32_t timeout_ms)
{
  uint32_t started_ms = HAL_GetTick();
  // 逆序恢复；任一失败保留门控，不能把部分恢复解释为正常运行。
  for (uint32_t i = UPGRADE_QUIESCE_COUNT; i > 0U; i--)
  {
    uint32_t module = i - 1U;
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
    UpgradeStageResult result = participant->resume(participant->context, remaining);
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
  taskENTER_CRITICAL();
  quiescing = false;
  taskEXIT_CRITICAL();
  return UPGRADE_STAGE_OK;
}
