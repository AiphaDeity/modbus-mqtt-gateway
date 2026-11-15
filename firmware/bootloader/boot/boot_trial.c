#include "boot_trial.h"
#include "upgrade_mailbox.h"
#include "boot_recovery.h"

#include <string.h>

static UpgradeMessage confirmation;  // 读出的消息
static bool have_confirmation;  // 是否接受为候选确认消息

// 接收并保存候选确认消息
void Boot_TrialBegin(volatile uint32_t *mailbox, bool software_reset)
{
  have_confirmation = software_reset && UpgradeMailbox_Read(mailbox, &confirmation)
                   && ((confirmation.kind == UPGRADE_MESSAGE_CONFIRM)
                       || (confirmation.kind == UPGRADE_MESSAGE_RECOVERY_CONFIRM));
  // 清空邮箱
  UpgradeMailbox_Clear(mailbox);
}

// 将普通确认消息与当前试运行事务身份逐项匹配。
bool Boot_TrialConfirmationMatches(const Boot_UpgradeState *state)
{
  return have_confirmation && (confirmation.kind == UPGRADE_MESSAGE_CONFIRM)
      && (state != NULL) && (state->phase == BOOT_UPGRADE_TRIAL)
      && (confirmation.sequence == state->sequence)
      && (confirmation.tx_id == state->tx_id)
      && (confirmation.trial_attempts == state->trial_attempts)
      && (memcmp(confirmation.digest, state->candidate.digest, 32U) == 0);
}

// 将恢复确认消息与当前恢复试运行身份逐项匹配。
bool Boot_TrialRecoveryConfirmationMatches(const UpgradeRecoveryRecord *state)
{
  return have_confirmation && (confirmation.kind == UPGRADE_MESSAGE_RECOVERY_CONFIRM)
      && (state != NULL) && (state->phase == UPGRADE_RECOVERY_TRIAL)
      && (confirmation.sequence == state->sequence) && (confirmation.tx_id == state->tx_id)
      && (confirmation.trial_attempts == state->trial_attempts)
      && (memcmp(confirmation.digest, state->package.digest, 32U) == 0);
}

// 根据当前持久状态写入正常或试运行交接消息。
bool Boot_TrialPrepareApp(volatile uint32_t *mailbox)
{
  if (mailbox == NULL)
  {
    return false;
  }
  Boot_RecoveryState recovery;
  Boot_UpgradeResult recovery_result = Boot_RecoveryLoad(&recovery);
  if (recovery_result != BOOT_UPGRADE_EMPTY)
  {
    if ((recovery_result != BOOT_UPGRADE_OK) || (recovery.phase != BOOT_RECOVERY_TRIAL))
    {
      return false;
    }
    UpgradeMessage message = {0};
    message.kind = UPGRADE_MESSAGE_RECOVERY_TRIAL;
    message.sequence = recovery.sequence;
    message.tx_id = recovery.tx_id;
    message.trial_attempts = recovery.trial_attempts;
    memcpy(message.digest, recovery.package.digest, sizeof(message.digest));
    UpgradeMailbox_Write(mailbox, &message);
    return true;
  }
  Boot_UpgradeState state;
  Boot_UpgradeResult result = Boot_UpgradeLoad(&state);
  UpgradeMessage message = {0};
  if (result == BOOT_UPGRADE_EMPTY)
  {
    return false;
  }
  else if ((result == BOOT_UPGRADE_OK) && (state.phase == BOOT_UPGRADE_CONFIRMED))
  {
    message.kind = UPGRADE_MESSAGE_NORMAL;
    message.sequence = state.sequence;
    message.tx_id = state.tx_id;
    message.trial_attempts = state.trial_attempts;
    memcpy(message.digest, state.confirmed.digest, sizeof(message.digest));
  }
  else if ((result == BOOT_UPGRADE_OK) && (state.phase == BOOT_UPGRADE_TRIAL))
  {
    message.kind = UPGRADE_MESSAGE_TRIAL;
    message.sequence = state.sequence;
    message.tx_id = state.tx_id;
    message.trial_attempts = state.trial_attempts;
    memcpy(message.digest, state.candidate.digest, sizeof(message.digest));
  }
  else
  {
    return false;
  }
  UpgradeMailbox_Write(mailbox, &message);
  return true;
}
