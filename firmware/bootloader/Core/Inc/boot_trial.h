#ifndef BOOT_TRIAL_H
#define BOOT_TRIAL_H

#include "boot_upgrade_state.h"
#include "upgrade_mailbox.h"

// main最早的USER CODE Init调用一次；仅软件复位接受确认请求，读取后消费邮箱。
// mailbox为独占的16字缓冲，仅调用期间有效，不保留指针。
void Boot_TrialBegin(volatile uint32_t *mailbox, bool software_reset);

// 将确认消息与普通 TRIAL 的序号、摘要及基线 2 事务/次数匹配；通过后仍须认证包和 App。
bool Boot_TrialConfirmationMatches(const Boot_UpgradeState *state);
// 按相同身份规则检查恢复试运行的确认消息，拒绝普通试运行消息。
bool Boot_TrialRecoveryConfirmationMatches(const UpgradeRecoveryRecord *state);

// 跳转前发布正常/试运行消息；状态异常返回false，不放行App。
bool Boot_TrialPrepareApp(volatile uint32_t *mailbox);

#endif
