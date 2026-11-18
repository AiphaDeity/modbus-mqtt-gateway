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

/*
 * 串口 FINISH 收齐检查通过后调用，重新加载 RECEIVING 记录并从外部槽认证实际包。
 * 认证通过才保存 READY；只建立安装依据，不在本函数擦写片内 App 或启动新程序。
 */
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
  // 收包认证完成才记录 READY；尚未表示片内 App 已安装，更未表示健康确认。
  state.phase = BOOT_RECOVERY_READY;
  return Boot_RecoverySave(&state);
}

/*
 * 恢复健康确认已持久化后收尾：核对或重建普通 CONFIRMED，保存恢复 DONE，最后撤启动屏障。
 * 普通日志已接管当前可信包才撤屏障；收尾中断后仍按实际记录核对，不提前退回普通启动。
 */
static Boot_InstallResult CommitRecovery(Boot_RecoveryState *state)
{
  Boot_UpgradeState normal;
  Boot_UpgradeResult result = Boot_UpgradeLoad(&normal);
  if ((result == BOOT_UPGRADE_IO_ERROR) || (result == BOOT_UPGRADE_UNSUPPORTED))
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  // 1. 先判断上次是否已将同一事务、试运行次数和镜像提交到普通日志，已完成则不重复重建。
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
    // 2. 仅此处允许重建损坏普通日志；源包/App已认证，独立CONFIRMED事务已持久提交。
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
  // 3. 普通日志重建完成后先记录 DONE；它表示普通记录已接管，下一步才撤掉屏障。
  if (state->phase != BOOT_RECOVERY_DONE)
  {
    state->phase = BOOT_RECOVERY_DONE;
    if (Boot_RecoverySave(state) != BOOT_UPGRADE_OK)
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
  }
  // 4. 最后擦除屏障扇区；下次 Boot_RecoveryLoad 才按 EMPTY 交回普通启动状态机。
  if (Boot_FlashRecoveryJournalErase(FLASH_PART_UPGLOG_BASE + BOOT_RECOVERY_BARRIER_OFFSET) != HAL_OK)
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  return BOOT_INSTALL_BOOT_EXISTING;
}

/*
 * Boot_InstallRun 优先调用的独立恢复状态机，使用 USART2 收包流程建立的恢复记录。
 * 恢复确认后重建普通 CONFIRMED 记录；未完成或 BLOCKED 时保持恢复屏障，交回 main 维护。
 * 这是独立恢复记录的处理链，不套用普通候选失败时 Restore 旧包的回退策略。
 */
Boot_InstallResult Boot_RecoveryRun(const uint8_t *public_key, const uint8_t *aes_key)
{
  if (Boot_WatchdogFailed())
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  // 先读取恢复屏障及记录；没有活动恢复事务才返回 NO_RECOVERY，让上层处理普通升级。
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
    // 半包或试运行耗尽都不能启动恢复候选，保留屏障等待维护工具重新恢复。
    return BOOT_INSTALL_STATE_ERROR;
  }
  // READY 以及安装/试运行/收尾各阶段都先重验恢复源，记录有效不等于包内容可信。
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
    // 恢复确认也必须核对片内真实镜像，并匹配 RECOVERY_CONFIRM 的事务、序号、次数和摘要。
    bool app_matches = Boot_InstallAppMatches(&state.package);
    if (!Boot_TrialRecoveryConfirmationMatches(&state)
        || !app_matches)
    {
      if ((state.trial_attempts < UPGRADE_TRIAL_LIMIT) && app_matches)
      {
        // 首次之外仍有机会且实际镜像正确，才持久计入下一次放行；不在这里重复擦装 App。
        state.trial_attempts++;
        return Boot_RecoverySave(&state) == BOOT_UPGRADE_OK
             ? BOOT_INSTALL_TRIAL_STARTED : BOOT_INSTALL_STATE_ERROR;
      }
      // 次数耗尽或片内不匹配就阻止继续试运行，保留独立恢复屏障等待新的显式恢复请求。
      state.phase = BOOT_RECOVERY_BLOCKED;
      // BLOCKED便于诊断；即使保存失败，原TRIAL也不会再次放行未确认App。
      (void)Boot_RecoverySave(&state);
      return BOOT_INSTALL_STATE_ERROR;
    }
    // 先把恢复健康结果独立保存为 CONFIRMED，再让普通日志接管，不能反过来清屏障。
    state.phase = BOOT_RECOVERY_CONFIRMED;
    if (Boot_RecoverySave(&state) != BOOT_UPGRADE_OK)
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
    return CommitRecovery(&state);
  }
  if (state.phase == BOOT_RECOVERY_READY)
  {
    // 先持久登记 INSTALLING，之后才能擦片内；断电后按此记录从恢复包完整重装。
    state.phase = BOOT_RECOVERY_INSTALLING;
    if (Boot_RecoverySave(&state) != BOOT_UPGRADE_OK)
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
  }
  // READY 已推进 INSTALLING，或前次安装中断；复用公共安装器写唯一的片内 App 区。
  if (!Boot_InstallImage(&source, &state.package, public_key, aes_key))
  {
    return BOOT_INSTALL_APP_ERROR;
  }
  // 安装成功先保存首次 TRIAL 放行，再回 main 回复第二条 FINISH 结果并准备启动交接。
  state.phase = BOOT_RECOVERY_TRIAL;
  state.trial_attempts = 1U;
  return (Boot_RecoverySave(&state) == BOOT_UPGRADE_OK)
       ? BOOT_INSTALL_TRIAL_STARTED : BOOT_INSTALL_STATE_ERROR;
}
