#include "boot_recovery.h"
#include "boot_trial.h"
#include "boot_watchdog.h"
#include "flash_partition.h"

#include <string.h>

// 将恢复记录中的包槽和长度映射为外部 Flash 源。
static Boot_PackageFlashSource RecoverySource(const Boot_RecoveryState *state)
{
  Boot_PackageFlashSource source;
  source.address = (state->package.slot == 1U) ? FLASH_PART_FW_A_BASE : FLASH_PART_FW_B_BASE;
  source.size = state->package.size;
  return source;
}

// 认证恢复包，并核对其与恢复记录的描述一致。
static bool VerifyRecovery(const Boot_RecoveryState *state, const uint8_t *public_key,
                            const uint8_t *aes_key, Boot_PackageFlashSource *source)
{
  *source = RecoverySource(state);
  Boot_UpgradePackage actual;
  return Boot_InstallVerifySource(source, public_key, aes_key, &actual, NULL)
      && (memcmp(actual.digest, state->package.digest, sizeof(actual.digest)) == 0);
}

// 认证已收完的恢复包，将事务持久推进至 READY。
Boot_UpgradeResult Boot_RecoveryAccept(const uint8_t *public_key, const uint8_t *aes_key)
{
  Boot_RecoveryState state;
  Boot_UpgradeResult result = Boot_RecoveryLoad(&state);
  if (result != BOOT_UPGRADE_OK)
  {
    return result;
  }
  if (state.phase != BOOT_RECOVERY_RECEIVING)
  {
    return BOOT_UPGRADE_INVALID;
  }
  Boot_PackageFlashSource source = RecoverySource(&state);
  if (!Boot_InstallVerifySource(&source, public_key, aes_key, &state.package, NULL))
  {
    return BOOT_UPGRADE_INVALID;
  }
  state.phase = BOOT_RECOVERY_READY;
  return Boot_RecoverySave(&state);
}

// 恢复CONFIRMED记录仍保持有效屏障；重建普通日志中断时，下次继续此流程。
static Boot_InstallResult CommitRecovery(Boot_RecoveryState *state)
{
  Boot_UpgradeState normal;
  Boot_UpgradeResult result = Boot_UpgradeLoad(&normal);
  if ((result == BOOT_UPGRADE_IO_ERROR) || (result == BOOT_UPGRADE_UNSUPPORTED))
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  bool already_committed = (result == BOOT_UPGRADE_OK)
                        && (normal.tx_id == state->tx_id)
                        && (normal.trial_attempts == state->trial_attempts)
                        && (normal.operation == state->operation)
                        && (normal.phase == BOOT_UPGRADE_CONFIRMED)
                        && (normal.confirmed.slot == state->package.slot)
                        && (normal.confirmed.size == state->package.size)
                        && (memcmp(normal.confirmed.digest, state->package.digest, 32U) == 0);
  if (!already_committed)
  {
    // 仅此处允许重建损坏普通日志；源包/App已认证，独立CONFIRMED事务已持久提交。
    if ((Boot_FlashJournalErase(FLASH_PART_UPGLOG_BASE) != HAL_OK)
        || (Boot_FlashJournalErase(FLASH_PART_UPGLOG_BASE + 4096U) != HAL_OK))
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
    memset(&normal, 0, sizeof(normal));
    normal.phase = BOOT_UPGRADE_CONFIRMED;
    normal.confirmed = state->package;
    normal.tx_id = state->tx_id;
    normal.trial_attempts = state->trial_attempts;
    normal.operation = state->operation;
    if (Boot_UpgradeSave(&normal) != BOOT_UPGRADE_OK)
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
  }
  if (state->phase != BOOT_RECOVERY_DONE)
  {
    state->phase = BOOT_RECOVERY_DONE;
    if (Boot_RecoverySave(state) != BOOT_UPGRADE_OK)
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
  }
  if (Boot_FlashRecoveryJournalErase(FLASH_PART_UPGLOG_BASE + BOOT_RECOVERY_BARRIER_OFFSET) != HAL_OK)
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  return BOOT_INSTALL_BOOT_EXISTING;
}

// 推进恢复安装和试运行，确认后重建普通已确认状态。
Boot_InstallResult Boot_RecoveryRun(const uint8_t *public_key, const uint8_t *aes_key)
{
  if (Boot_WatchdogFailed())
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  Boot_RecoveryState state;
  Boot_UpgradeResult result = Boot_RecoveryLoad(&state);
  if (result == BOOT_UPGRADE_EMPTY)
  {
    return BOOT_INSTALL_NO_RECOVERY;
  }
  if (result != BOOT_UPGRADE_OK)
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  if ((state.phase == BOOT_RECOVERY_RECEIVING) || (state.phase == BOOT_RECOVERY_BLOCKED))
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  Boot_PackageFlashSource source;
  if (!VerifyRecovery(&state, public_key, aes_key, &source))
  {
    return BOOT_INSTALL_SOURCE_ERROR;
  }
  if ((state.phase == BOOT_RECOVERY_CONFIRMED) || (state.phase == BOOT_RECOVERY_DONE))
  {
    // 健康已确认后仍可能出现片内损坏；重装同一可信镜像，不能卡在无法继续提交的状态。
    if (!Boot_InstallAppMatches(&state.package)
        && !Boot_InstallImage(&source, &state.package, public_key, aes_key))
    {
      return BOOT_INSTALL_APP_ERROR;
    }
    return CommitRecovery(&state);
  }
  if (state.phase == BOOT_RECOVERY_TRIAL)
  {
    bool app_matches = Boot_InstallAppMatches(&state.package);
    if (!Boot_TrialRecoveryConfirmationMatches(&state)
        || !app_matches)
    {
      if ((state.trial_attempts < UPGRADE_TRIAL_LIMIT) && app_matches)
      {
        state.trial_attempts++;
        return Boot_RecoverySave(&state) == BOOT_UPGRADE_OK
             ? BOOT_INSTALL_TRIAL_STARTED : BOOT_INSTALL_STATE_ERROR;
      }
      state.phase = BOOT_RECOVERY_BLOCKED;
      // BLOCKED便于诊断；即使保存失败，原TRIAL也不会再次放行未确认App。
      (void)Boot_RecoverySave(&state);
      return BOOT_INSTALL_STATE_ERROR;
    }
    state.phase = BOOT_RECOVERY_CONFIRMED;
    if (Boot_RecoverySave(&state) != BOOT_UPGRADE_OK)
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
    return CommitRecovery(&state);
  }
  if (state.phase == BOOT_RECOVERY_READY)
  {
    state.phase = BOOT_RECOVERY_INSTALLING;
    if (Boot_RecoverySave(&state) != BOOT_UPGRADE_OK)
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
  }
  if (!Boot_InstallImage(&source, &state.package, public_key, aes_key))
  {
    return BOOT_INSTALL_APP_ERROR;
  }
  state.phase = BOOT_RECOVERY_TRIAL;
  state.trial_attempts = 1U;
  return (Boot_RecoverySave(&state) == BOOT_UPGRADE_OK)
       ? BOOT_INSTALL_TRIAL_STARTED : BOOT_INSTALL_STATE_ERROR;
}
