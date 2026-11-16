#include "boot_watchdog.h"

#include "boot_install.h"
#include "boot_recovery.h"
#include "flash_partition.h"
#include "watchdog.h"
#include "stm32f4xx_hal.h"

#include <string.h>
#include <stdio.h>

static bool watchdog_started;
static bool watchdog_failed;
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

// 启动 Boot 看门狗并记录初始时基，失败时锁存故障。
bool Boot_WatchdogStart(void)
{
  // 已经锁存失败就返回 false
  if (watchdog_failed)
  {
    return false;
  }
  // 已经启动就返回 true
  if (watchdog_started)
  {
    return true;
  }

#if defined(DEBUG)
  // 调试器暂停 CPU 时，冻结 IWDG 计数
  bool ok = Watchdog_Start(true);
#else
  bool ok = Watchdog_Start(false);
#endif
  if (!ok)
  {
    watchdog_failed = true;
    return false;
  }

  watchdog_started = true;
  last_refresh_ms = HAL_GetTick();
  return true;
}

// 检查操作期限，失败后锁存本次监督故障。
bool Boot_WatchdogCheck(uint32_t started_ms, uint32_t timeout_ms)
{
  if (watchdog_failed)
  {
    return false;
  }

  uint32_t elapsed_ms = (uint32_t)(HAL_GetTick() - started_ms);
  if ((timeout_ms == 0U) || (timeout_ms > 0x7FFFFFFFU)
      || (elapsed_ms >= timeout_ms))
  {
    watchdog_failed = true;
    printf("WDG DEADLINE elapsed_ms=%lu limit_ms=%lu\r\n",
           (unsigned long)elapsed_ms, (unsigned long)timeout_ms);
    return false;
  }

  return true;
}

// 检查期限并在时基前进时刷新看门狗。
bool Boot_WatchdogProgress(uint32_t started_ms, uint32_t timeout_ms)
{
  // 	检查是否超时、是否已经锁存故障
  if (!Boot_WatchdogCheck(started_ms, timeout_ms))
  {
    return false;
  }

  return Boot_WatchdogIdleProgress();
}

// 	检查允许后，在时基前进时喂狗
bool Boot_WatchdogIdleProgress(void)
{
  if (watchdog_failed)
  {
    return false;
  }

  uint32_t now_ms = HAL_GetTick();
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
  if (!watchdog_started || watchdog_failed || !Watchdog_IsSoftwareMode())
  {
    return false;
  }
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
  if (!Boot_InstallVerifySource(&source, public_key, aes_key, &actual, &capabilities)
      || (capabilities != BOOT_PACKAGE_CAP_IWDG_HANDOFF)
      || (actual.slot != expected.slot) || (actual.size != expected.size)
      || (memcmp(actual.digest, expected.digest, sizeof(actual.digest)) != 0)
      || !Boot_InstallAppMatches(&expected))
  {
    return false;
  }
  // main已提前启狗，此处只完成最终放行，不补启或重新配置IWDG。
  return !watchdog_failed;
}
