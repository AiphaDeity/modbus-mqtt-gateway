#include "app_readiness.h"

#include "FreeRTOS.h"
#include "event_groups.h"
#include "cmsis_os2.h"

static StaticEventGroup_t readiness_control_block;
static osEventFlagsId_t readiness_events;

static const osEventFlagsAttr_t readiness_attributes =
{
  .name = "app_ready",
  .cb_mem = &readiness_control_block,
  .cb_size = sizeof(readiness_control_block)
};

// 只接受已定义且非空的资源集合，避免把拼错的掩码当作就绪条件
static bool App_ResourcesValid(uint32_t resources)
{
  return (resources != 0U) && ((resources & ~APP_RESOURCE_ALL) == 0U);
}

/*
 * 启动任务在模块初始化前调用，使用静态控制块建立初始全未就绪的事件组。
 * 内核未运行或创建失败返回 false；重复调用保留现有事件组和资源状态。
 */
bool App_ReadinessInit(void)
{
  if (readiness_events != NULL)
  {
    return true;
  }

  if (osKernelGetState() != osKernelRunning)
  {
    return false;
  }

  readiness_events = osEventFlagsNew(&readiness_attributes);
  return readiness_events != NULL;
}

/*
 * 由资源提供方在任务上下文报告单项资源的可用或失效，不修改其他资源位。
 * 非法资源、事件组未建立或 RTOS 更新失败时返回 false，供调用方处理。
 */
bool App_SetResourceReady(AppResource resource, bool ready)
{
  uint32_t bits = (uint32_t)resource;

  if ((readiness_events == NULL) || !App_ResourcesValid(bits) ||
      ((bits & (bits - 1U)) != 0U))
  {
    return false;
  }

  uint32_t result;
  if (ready)
  {
    result = osEventFlagsSet(readiness_events, bits);
  }
  else
  {
    result = osEventFlagsClear(readiness_events, bits);
  }

  return (result & osFlagsError) == 0U;
}

// 获取一次就绪位快照；未建立事件组或查询失败时安全地按全部未就绪返回
uint32_t App_GetReadyResources(void)
{
  if (readiness_events == NULL)
  {
    return 0U;
  }

  uint32_t result = osEventFlagsGet(readiness_events);
  if ((result & osFlagsError) != 0U)
  {
    return 0U;
  }

  return result & APP_RESOURCE_ALL;
}

// 查询任务所需资源是否在本次快照中全部就绪；非法条件不视为已满足
bool App_IsReady(uint32_t required_resources)
{
  if (!App_ResourcesValid(required_resources))
  {
    return false;
  }

  return (App_GetReadyResources() & required_resources) == required_resources;
}

/*
 * 任务按毫秒上限等待全部所需资源，0 表示立即检查；保留事件位供其他任务观察。
 * 校验条件及时间换算范围，分别返回就绪、超时、参数错误或 RTOS 错误。
 * 就绪结果是一次观察，调用方在实际业务操作前仍要复验并处理资源失效。
 */
AppWaitResult App_WaitResources(uint32_t required_resources, uint32_t timeout_ms)
{
  if (!App_ResourcesValid(required_resources) || (timeout_ms == UINT32_MAX))
  {
    return APP_WAIT_INVALID_ARGUMENT;
  }

  if (readiness_events == NULL)
  {
    return APP_WAIT_ERROR;
  }

  uint32_t frequency = osKernelGetTickFreq();
  if (frequency == 0U)
  {
    return APP_WAIT_ERROR;
  }

  // 毫秒向上取整为内核 tick，防止短于 1 tick 的等待变成零等待
  uint64_t ticks = ((uint64_t)timeout_ms * frequency + 999U) / 1000U;
  if (ticks >= osWaitForever)
  {
    return APP_WAIT_INVALID_ARGUMENT;
  }

  uint32_t result = osEventFlagsWait(readiness_events, required_resources,
                                    osFlagsWaitAll | osFlagsNoClear, (uint32_t)ticks);
  if ((result & osFlagsError) == 0U)
  {
    return ((result & required_resources) == required_resources) ?
      APP_WAIT_READY : APP_WAIT_ERROR;
  }

  if ((result == osFlagsErrorTimeout) || (result == osFlagsErrorResource))
  {
    return APP_WAIT_TIMEOUT;
  }

  return APP_WAIT_ERROR;
}
