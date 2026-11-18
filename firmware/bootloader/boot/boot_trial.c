#include "boot_trial.h"
#include "upgrade_mailbox.h"
#include "boot_recovery.h"

#include <string.h>

static UpgradeMessage confirmation;  // 读出的消息
static bool have_confirmation;  // 是否接受为候选确认消息

/*
 * Boot main 保存复位原因后调用，接收上一次 App 经邮箱发来的健康确认意图。
 * 本函数只保存候选确认和消费旧消息；是否正式接受由后续安装状态机核对事务及镜像。
 */
void Boot_TrialBegin(volatile uint32_t *mailbox, bool software_reset)
{
  // 1. software_reset 为真才继续读取邮箱，掉电、欠压或看门狗复位不采信留下的确认。
  // 2. Read 检查格式和 CRC，并解码到 confirmation；这里只接受普通或恢复的 CONFIRM 消息。
  have_confirmation = software_reset && UpgradeMailbox_Read(mailbox, &confirmation)
                   && ((confirmation.kind == UPGRADE_MESSAGE_CONFIRM)
                       || (confirmation.kind == UPGRADE_MESSAGE_RECOVERY_CONFIRM));
  // 3. 无论本次是否接受，都清除邮箱有效标记，防止旧消息被下一次启动重复使用。
  // 只使消息失效，不把整个保留区清零；有效消息已保存在 Boot 的 confirmation 变量中。
  UpgradeMailbox_Clear(mailbox);
}

/*
 * 将已接收的普通确认与当前 TRIAL 记录绑定：消息类型、记录序号、事务号、放行次数和摘要须相同。
 * 防止其他事务或前一次试运行的旧消息被采信；匹配后仍由 Boot_InstallRun 重验包及片内 App。
 */
bool Boot_TrialConfirmationMatches(const Boot_UpgradeState *state)
{
  return have_confirmation && (confirmation.kind == UPGRADE_MESSAGE_CONFIRM)
      && (state != NULL) && (state->phase == BOOT_UPGRADE_TRIAL)
      && (confirmation.sequence == state->sequence)
      && (confirmation.tx_id == state->tx_id)
      && (confirmation.trial_attempts == state->trial_attempts)
      && (memcmp(confirmation.digest, state->candidate.digest, 32U) == 0);
}

/*
 * 仅将 RECOVERY_CONFIRM 与当前独立恢复 TRIAL 记录比较，不拿普通 CONFIRM 消息代替。
 * 序号、事务号、放行次数和镜像摘要都须相同，防止其他恢复事务或前一次试运行的消息被复用。
 */
bool Boot_TrialRecoveryConfirmationMatches(const UpgradeRecoveryRecord *state)
{
  return have_confirmation && (confirmation.kind == UPGRADE_MESSAGE_RECOVERY_CONFIRM)
      && (state != NULL) && (state->phase == UPGRADE_RECOVERY_TRIAL)
      && (confirmation.sequence == state->sequence) && (confirmation.tx_id == state->tx_id)
      && (confirmation.trial_attempts == state->trial_attempts)
      && (memcmp(confirmation.digest, state->package.digest, 32U) == 0);
}

/*
 * Boot main 最终交接前调用，将持久记录转换为 App 要读取的 NORMAL/TRIAL 启动身份。
 * 试运行机会已由安装状态机登记；这里只重新读取保存后的序号和次数，不再消耗一次机会。
 * 此处向邮箱写启动消息；App 健康后反向写确认消息，下一次 Boot_TrialBegin 再读取。
 */
bool Boot_TrialPrepareApp(volatile uint32_t *mailbox)
{
  if (mailbox == NULL)
  {
    return false;
  }
  // 独立恢复事务优先；存在恢复屏障时，不能退而使用普通日志冒充正常启动。
  Boot_RecoveryState recovery;
  Boot_UpgradeResult recovery_result = Boot_RecoveryLoad(&recovery);
  if (recovery_result != BOOT_UPGRADE_EMPTY)
  {
    // 恢复记录读取失败或阶段不是恢复试运行：持久状态与预期不符，拒绝交接。
    if ((recovery_result != BOOT_UPGRADE_OK) || (recovery.phase != BOOT_RECOVERY_TRIAL))
    {
      return false;
    }
    // 写恢复试运行消息：携带恢复事务身份与候选包摘要，App 据此进入恢复试运行上报。
    UpgradeMessage message = {0};
    message.kind = UPGRADE_MESSAGE_RECOVERY_TRIAL;
    message.sequence = recovery.sequence;
    message.tx_id = recovery.tx_id;
    message.trial_attempts = recovery.trial_attempts;
    memcpy(message.digest, recovery.package.digest, sizeof(message.digest));
    UpgradeMailbox_Write(mailbox, &message);
    return true;
  }
  // 无恢复事务时按普通升级日志决定交接内容。
  Boot_UpgradeState state;
  Boot_UpgradeResult result = Boot_UpgradeLoad(&state);
  UpgradeMessage message = {0};
  if (result == BOOT_UPGRADE_EMPTY)
  {
    // 没有任何升级日志：无法建立交接身份，拒绝交接。
    return false;
  }
  else if ((result == BOOT_UPGRADE_OK) && (state.phase == BOOT_UPGRADE_CONFIRMED))
  {
    // 已确认固件：写普通启动消息，摘要取自已确认记录。
    message.kind = UPGRADE_MESSAGE_NORMAL;
    message.sequence = state.sequence;
    message.tx_id = state.tx_id;
    message.trial_attempts = state.trial_attempts;
    memcpy(message.digest, state.confirmed.digest, sizeof(message.digest));
  }
  else if ((result == BOOT_UPGRADE_OK) && (state.phase == BOOT_UPGRADE_TRIAL))
  {
    // 试运行中：写试运行消息，摘要取自候选槽，健康确认由 App 写邮箱并请求软件复位完成。
    message.kind = UPGRADE_MESSAGE_TRIAL;
    message.sequence = state.sequence;
    message.tx_id = state.tx_id;
    message.trial_attempts = state.trial_attempts;
    memcpy(message.digest, state.candidate.digest, sizeof(message.digest));
  }
  else
  {
    // 日志读取失败或阶段未知：持久状态不可信，拒绝交接。
    return false;
  }
  // 	告诉 App 本次启动身份
  UpgradeMailbox_Write(mailbox, &message);
  return true;
}
