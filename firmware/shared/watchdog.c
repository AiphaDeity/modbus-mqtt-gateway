#include "watchdog.h"

#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"

/*
 * Boot 与 App 共用硬件 IWDG 操作；启动后跨跳转继续计数，不能靠关闭外设绕过监督。
 * 本文件只负责启用和刷新，是否允许喂狗由各阶段的监督代码判断。
 */
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

/*
 * Boot 启动、App 早期接管同一个硬件 IWDG，配置分频、重装值及调试暂停行为。
 * 返回前等待配置生效并首次重装；后续是否继续刷新，由 Boot 检查点或 App Monitor 决定。
 */
bool Watchdog_Start(bool freeze_on_debug)
{
  // 1. 只读检查软件启动模式，配置不符就失败，不在启动过程中改选项字节。
  if (!Watchdog_IsSoftwareMode())
  {
    return false;
  }

  // 2. 参数只控制调试器暂停 CPU 时的冻结行为；DEBUG 固件正常运行时仍必须按条件喂狗。
  if (freeze_on_debug)
  {
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;
  }
  else
  {
    DBGMCU->APB1FZ &= ~DBGMCU_APB1_FZ_DBG_IWDG_STOP;
  }

  // 3. 启动会强制开启 LSI；即使随后失败，也不能在本次运行中关闭 IWDG。
  // App 再执行本流程仍操作同一组 IWDG 寄存器，Boot 到 App 的跳转不会创建或切换另一只看门狗。
  IWDG->KR = IWDG_KEY_START;  // 启动IWDG
  IWDG->KR = IWDG_KEY_WRITE;  // 允许修改配置
  // 当前名义超时为 (4095 + 1) × 256 / LSI频率；假定 LSI=32 kHz 时为 32.768 s，实际随偏差变化。
  IWDG->PR = IWDG_PR_PR_2 | IWDG_PR_PR_1; // 编码6表示256分频。
  IWDG->RLR = IWDG_RELOAD_VALUE;  //设置重装值

  // 4. 等待分频和重装寄存器更新完成，共用本次配置的固定期限。
  uint32_t started_ms = HAL_GetTick();
  while ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0U)
  {
    if ((uint32_t)(HAL_GetTick() - started_ms) >= IWDG_UPDATE_TIMEOUT_MS)
    {
      return false;
    }
    // 等待期间不喂狗；若连HAL时基也停住，由已启动的IWDG终止等待。
  }

  // 5. 配置生效后首次重装计数；它只建立本阶段起点，不代表后续业务已通过监督。
  Watchdog_Refresh();
  return true;
}

// 只写重装密钥刷新硬件计数，不检查任务、超时或健康状态；调用方必须先取得喂狗资格。
void Watchdog_Refresh(void)
{
  IWDG->KR = IWDG_KEY_RELOAD;
}
