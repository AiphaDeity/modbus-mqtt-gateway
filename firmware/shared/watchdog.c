#include "watchdog.h"

#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"

// F407的IWDG钥匙值由参考手册定义，不依赖是否启用HAL IWDG模块。
#define IWDG_KEY_START 0xCCCCU
#define IWDG_KEY_WRITE 0x5555U
#define IWDG_KEY_RELOAD 0xAAAAU
#define IWDG_RELOAD_VALUE 4095U
#define IWDG_UPDATE_TIMEOUT_MS 1000U

// 读取选项字节，检查 IWDG 是否采用软件启动模式。
bool Watchdog_IsSoftwareMode(void)
{
  return (FLASH->OPTCR & FLASH_OPTCR_WDG_SW) != 0U;
}

// 启动和配置硬件 IWDG，配置调试暂停行为及 IWDG 分频/重装值，等待寄存器更新。
bool Watchdog_Start(bool freeze_on_debug)
{
  // 只检查选项字节，确认符合工程要求的软件启动模式
  if (!Watchdog_IsSoftwareMode())
  {
    return false;
  }

  if (freeze_on_debug)
  {
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;
  }
  else
  {
    DBGMCU->APB1FZ &= ~DBGMCU_APB1_FZ_DBG_IWDG_STOP;
  }

  // 启动会强制开启LSI；即使随后失败，也不能在本次运行中关闭IWDG。
  IWDG->KR = IWDG_KEY_START;  // 启动IWDG
  IWDG->KR = IWDG_KEY_WRITE;  // 允许修改配置
  IWDG->PR = IWDG_PR_PR_2 | IWDG_PR_PR_1; // 编码6表示256分频。
  IWDG->RLR = IWDG_RELOAD_VALUE;  //设置重装值

  uint32_t started_ms = HAL_GetTick();
  while ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0U)
  {
    if ((uint32_t)(HAL_GetTick() - started_ms) >= IWDG_UPDATE_TIMEOUT_MS)
    {
      return false;
    }
    // 等待期间不喂狗；若连HAL时基也停住，由已启动的IWDG终止等待。
  }

  // 开始喂狗
  Watchdog_Refresh();
  return true;
}

// 写入重装密钥，刷新已启动的 IWDG。
void Watchdog_Refresh(void)
{
  IWDG->KR = IWDG_KEY_RELOAD;
}
