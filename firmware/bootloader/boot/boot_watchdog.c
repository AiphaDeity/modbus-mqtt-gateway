#include "boot_watchdog.h"

#include "boot_install.h"
#include "boot_recovery.h"
#include "flash_partition.h"
#include "watchdog.h"
#include "stm32f4xx_hal.h"

#include <string.h>
#include <stdio.h>

static bool watchdog_started;
static bool watchdog_failed; // 一旦锁存，本次 Boot 不再恢复喂狗，由主入口处理复位。
static uint32_t last_refresh_ms;

// 启用 DWT 周期计数，不清除已累计周期。
bool Boot_WatchdogEnableCycleCounter(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U)
  {
    return false;
  }
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  return (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U;
}

/*
 * Boot main 建立日志和存储通道后调用，启用后续准备、安装及维护操作的 IWDG 监督。
 * 成功记录开始状态，后续操作通过 Check/Progress 报告期限与进展，不能无条件喂狗。
 */
bool Boot_WatchdogStart(void)
{
  // 1. 已失败的本次启动不重新放行；已有正常启动记录则直接返回，不重复配置。
  if (watchdog_failed)
  {
    return false;
  }
  if (watchdog_started)
  {
    return true;
  }

#if defined(DEBUG)
  // 2. 共用 Watchdog_Start 配置硬件 IWDG；DEBUG 允许调试暂停时冻结计数。
  bool ok = Watchdog_Start(true);
#else
  bool ok = Watchdog_Start(false);
#endif
  if (!ok)
  {
    watchdog_failed = true;
    return false;
  }

  // 3. 硬件启用成功才发布已启动状态，保存 HAL 时刻供后续检查点控制刷新。
  watchdog_started = true;
  last_refresh_ms = HAL_GetTick();
  return true;
}

// 只检查并锁存软件期限故障，不刷新 IWDG；started_ms/timeout_ms 由具体操作提供，单位 ms。
bool Boot_WatchdogCheck(uint32_t started_ms, uint32_t timeout_ms)
{
  if (watchdog_failed)
  {
    return false;
  }

  // 起点由具体操作保存，不因调用本检查函数而更新；无符号差值允许毫秒计数回绕。
  uint32_t elapsed_ms = (uint32_t)(HAL_GetTick() - started_ms);
  if ((timeout_ms == 0U) || (timeout_ms > 0x7FFFFFFFU)
      || (elapsed_ms >= timeout_ms))
  {
    // 参数非法或到期就锁存；本次 Boot 后续的 Check/Progress 均不能把故障清回正常。
    watchdog_failed = true;
    printf("WDG DEADLINE elapsed_ms=%lu limit_ms=%lu\r\n",
           (unsigned long)elapsed_ms, (unsigned long)timeout_ms);
    return false;
  }

  return true;
}

/*
 * 调用方完成真实工作后报告检查点，例如读出一块并更新摘要，或完成一次擦写及读回。
 * 先检查原固定总期限，再按 HAL 时基变化决定刷新；本函数不会自行判断业务是否真正前进。
 */
bool Boot_WatchdogProgress(uint32_t started_ms, uint32_t timeout_ms)
{
  // 先检查本操作总期限；失败后不能通过后面的刷新把故障掩盖掉。
  if (!Boot_WatchdogCheck(started_ms, timeout_ms))
  {
    return false;
  }

  return Boot_WatchdogIdleProgress();
}

// 在监督允许且时基已前进时刷新 IWDG，避免卡在同一时刻的重复调用被算作进展。
bool Boot_WatchdogIdleProgress(void)
{
  if (watchdog_failed)
  {
    return false;
  }

  uint32_t now_ms = HAL_GetTick();
  // 时基变化只防止同一时刻重复刷新，不是业务进展证明；本函数也不会更新外层操作的起点。
  if (watchdog_started && (now_ms != last_refresh_ms))
  {
    Watchdog_Refresh();
    last_refresh_ms = now_ms;
  }

  return true;
}

// 查询本次启动是否已锁存监督失败。
bool Boot_WatchdogFailed(void)
{
  return watchdog_failed;
}

// 恢复事务优先；每个可启动状态都必须明确指向一份可信包。
static bool SelectPackage(Boot_UpgradePackage *package)
{
  Boot_RecoveryState recovery;
  Boot_UpgradeResult result = Boot_RecoveryLoad(&recovery);
  if (result == BOOT_UPGRADE_OK)
  {
    if (recovery.phase != BOOT_RECOVERY_TRIAL)
    {
      return false;
    }
    *package = recovery.package;
    return true;
  }
  if (result != BOOT_UPGRADE_EMPTY)
  {
    return false;
  }

  Boot_UpgradeState normal;
  if (Boot_UpgradeLoad(&normal) != BOOT_UPGRADE_OK)
  {
    return false;
  }
  if (normal.phase == BOOT_UPGRADE_CONFIRMED)
  {
    *package = normal.confirmed;
  }
  else if (normal.phase == BOOT_UPGRADE_TRIAL)
  {
    *package = normal.candidate;
  }
  else
  {
    return false;
  }
  return true;
}

// 认证最终启动包并核对片内 App，确认看门狗可交接。
bool Boot_WatchdogPrepareApp(const uint8_t *public_key,
                             const uint8_t *aes_key)
{
  // 1. 先确认本次 Boot 的 IWDG 监督仍有效，已失败的启动不能继续交给 App。
  if (!watchdog_started || watchdog_failed || !Watchdog_IsSoftwareMode())
  {
    return false;
  }
  // 2. 从最终持久状态选择实际启动包：已确认状态选旧源，试运行状态选本次候选。
  Boot_UpgradePackage expected;
  if (!SelectPackage(&expected)
      || ((expected.slot != 1U) && (expected.slot != 2U)))
  {
    return false;
  }

  Boot_PackageFlashSource source;
  source.address = (expected.slot == 1U)
                 ? FLASH_PART_FW_A_BASE : FLASH_PART_FW_B_BASE;
  source.size = expected.size;
  Boot_UpgradePackage actual;
  uint16_t capabilities = 0U;
  // 3. 重新认证源包，核对 IWDG 交接声明、包身份及片内实际内容，任一失败都拒绝交接。
  if (!Boot_InstallVerifySource(&source, public_key, aes_key, &actual, &capabilities)
      || (capabilities != BOOT_PACKAGE_CAP_IWDG_HANDOFF)
      || (actual.slot != expected.slot) || (actual.size != expected.size)
      || (memcmp(actual.digest, expected.digest, sizeof(actual.digest)) != 0)
      || !Boot_InstallAppMatches(&expected))
  {
    return false;
  }
  // 4. 成功返回 main，由它调用 Boot_JumpToApp；此处不重新启狗，也不执行跳转。
  // IWDG 在清理外设、切换向量和 App 启动代码期间继续计数，直到 App main 早期接管。
  return !watchdog_failed;
}
