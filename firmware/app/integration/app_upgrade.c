#include "app_upgrade.h"
#include "app_modules.h"
#include "upgrade_stage.h"
#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

static UpgradeMessage boot_message;
static bool initialized;
static bool message_valid;
static bool trial;
static bool stable;
static uint32_t started_ms;
static uint32_t stable_since_ms;
static AppUpgradeAction pending_action;
static AppUpgradeResult boot_result;
static uint32_t result_polled_ms;
static bool result_resolved;

// 读取并消费 Boot 邮箱，建立本次启动身份和试运行起点。
void App_UpgradeInit(volatile uint32_t *mailbox, bool boot_managed, uint32_t now_ms)
{
  if (initialized)
  {
    return;
  }
  started_ms = now_ms;
  if (!boot_managed)
  {
    message_valid = true;
  }
  else
  {
    bool received = UpgradeMailbox_Read(mailbox, &boot_message);
    bool trial_message = (boot_message.kind == UPGRADE_MESSAGE_TRIAL)
                      || (boot_message.kind == UPGRADE_MESSAGE_RECOVERY_TRIAL);
    message_valid = received && ((boot_message.kind == UPGRADE_MESSAGE_NORMAL)
                                 || (trial_message && (boot_message.sequence != 0U)));
    trial = message_valid && trial_message;
    UpgradeMailbox_Clear(mailbox);
  }
  // initialized/message_valid/trial此后不改变，业务任务只读，无跨任务复合状态更新。
  initialized = true;
}

// 查询本次启动是否为有效的试运行。
bool App_UpgradeIsTrial(void)
{
  return initialized && trial;
}

// 仅在有效且非试运行的启动状态下开放业务提交。
bool App_UpgradeCanCommit(void)
{
  return initialized && message_valid && !trial;
}

// 返回 Boot 正常启动身份，供升级准备时核对恢复源。
bool App_UpgradeGetBootIdentity(UpgradeMessage *message)
{
  if (message == NULL)
  {
    return false;
  }
  memset(message, 0, sizeof(*message));
  if (!initialized || !message_valid || trial || (boot_message.kind != UPGRADE_MESSAGE_NORMAL)
      || (boot_message.sequence == 0U))
  {
    return false;
  }
  *message = boot_message;
  return true;
}

// 返回不可变的本次Boot交接身份，不放宽正常业务的提交门控。
bool App_UpgradeGetStartupIdentity(UpgradeMessage *message)
{
  if (message == NULL)
  {
    return false;
  }
  memset(message, 0, sizeof(*message));
  if (!initialized || !message_valid || (boot_message.sequence == 0U) || (boot_message.tx_id == 0U))
  {
    return false;
  }
  *message = boot_message;
  return true;
}

// 持久身份全部匹配后，按阶段和操作区分试运行、确认和回退；首次部署不算升级成功。
static AppUpgradeResult ClassifyResult(const UpgradeMessage *identity,
                                        const UpgradeStateProtection *state)
{
  if (identity->kind == UPGRADE_MESSAGE_RECOVERY_TRIAL)
  {
    const UpgradeRecoveryRecord *record = &state->recovery;
    bool normal_valid = (state->normal_result == UPGRADE_STATE_EMPTY)
                     || ((state->normal_result == UPGRADE_STATE_OK)
                         && UpgradeState_RecordValid(&state->normal));
    return normal_valid && (state->recovery_result == UPGRADE_STATE_OK) && UpgradeRecovery_RecordValid(record)
        && (record->phase == UPGRADE_RECOVERY_TRIAL) && (identity->sequence == record->sequence)
        && (identity->tx_id == record->tx_id) && (identity->trial_attempts == record->trial_attempts)
        && (memcmp(identity->digest, record->package.digest, sizeof(identity->digest)) == 0)
         ? APP_UPGRADE_RESULT_TRIAL : APP_UPGRADE_RESULT_UNKNOWN;
  }
  const UpgradeStateRecord *record = &state->normal;
  if ((state->normal_result != UPGRADE_STATE_OK) || (state->recovery_result != UPGRADE_STATE_EMPTY)
      || !UpgradeState_RecordValid(record) || (identity->sequence != record->sequence)
      || (identity->tx_id != record->tx_id) || (identity->trial_attempts != record->trial_attempts))
  {
    return APP_UPGRADE_RESULT_UNKNOWN;
  }
  if ((identity->kind == UPGRADE_MESSAGE_TRIAL) && (record->phase == UPGRADE_STATE_TRIAL)
      && (memcmp(identity->digest, record->candidate.digest, sizeof(identity->digest)) == 0))
  {
    return APP_UPGRADE_RESULT_TRIAL;
  }
  if ((identity->kind != UPGRADE_MESSAGE_NORMAL) || (record->phase != UPGRADE_STATE_CONFIRMED)
      || (memcmp(identity->digest, record->confirmed.digest, sizeof(identity->digest)) != 0))
  {
    return APP_UPGRADE_RESULT_UNKNOWN;
  }
  return ((record->operation == UPGRADE_OPERATION_INSTALL)
           || (record->operation == UPGRADE_OPERATION_RECOVERY)) ? APP_UPGRADE_RESULT_CONFIRMED
       : record->operation == UPGRADE_OPERATION_RESTORE ? APP_UPGRADE_RESULT_ROLLED_BACK
       : APP_UPGRADE_RESULT_UNKNOWN;
}

// 由现有升级任务读取一次可信结果；未就绪和读失败保持UNKNOWN，下一轮按期限重试。
void App_UpgradePollResult(uint32_t now_ms)
{
  if (!App_IsCurrentModuleTask(APP_MODULE_UPGRADE)
      || result_resolved
      || ((uint32_t)(now_ms - result_polled_ms) < 1000U))
  {
    return;
  }
  result_polled_ms = now_ms;
  UpgradeMessage identity;
  if (!App_UpgradeGetStartupIdentity(&identity))
  {
    result_resolved = true;
    return;
  }
  UpgradeStateProtection state;
  if (UpgradeStage_ReadBootState(&state, 3000U) != UPGRADE_STAGE_OK)
  {
    return;
  }
  AppUpgradeResult result = ClassifyResult(&identity, &state);
  taskENTER_CRITICAL();
  boot_result = result;
  result_resolved = true;
  taskEXIT_CRITICAL();
}

// 单字结果在短临界区内发布和读取，身份字段在任务创建前已固定。
AppUpgradeResult App_UpgradeGetResult(void)
{
  taskENTER_CRITICAL();
  AppUpgradeResult result = boot_result;
  taskEXIT_CRITICAL();
  return result;
}

// 根据总期限和连续健康时间决定等待、确认或回退复位。
AppUpgradeAction App_UpgradePoll(uint32_t now_ms, bool platform_healthy, bool product_healthy)
{
  if (pending_action != APP_UPGRADE_WAIT)
  {
    return pending_action;
  }
  if (!initialized)
  {
    return APP_UPGRADE_WAIT;
  }
  if (!message_valid)
  {
    pending_action = APP_UPGRADE_ROLLBACK_RESET;
    return pending_action;
  }
  if (!trial)
  {
    return APP_UPGRADE_WAIT;
  }
  if ((uint32_t)(now_ms - started_ms) >= APP_UPGRADE_DEADLINE_MS)
  {
    pending_action = APP_UPGRADE_ROLLBACK_RESET;
    return pending_action;
  }
  if (!platform_healthy || !product_healthy)
  {
    stable = false;
    return APP_UPGRADE_WAIT;
  }
  if (!stable)
  {
    stable = true;
    stable_since_ms = now_ms;
  }
  if ((uint32_t)(now_ms - stable_since_ms) >= APP_UPGRADE_STABLE_MS)
  {
    pending_action = APP_UPGRADE_CONFIRM_RESET;
  }
  return pending_action;
}

// 将当前试运行身份写成健康确认消息，供复位后 Boot 验证。
bool App_UpgradeWriteConfirmation(volatile uint32_t *mailbox)
{
  if ((mailbox == 0) || (pending_action != APP_UPGRADE_CONFIRM_RESET) || !trial)
  {
    return false;
  }
  UpgradeMessage request = boot_message;
  request.kind = (boot_message.kind == UPGRADE_MESSAGE_RECOVERY_TRIAL)
               ? UPGRADE_MESSAGE_RECOVERY_CONFIRM : UPGRADE_MESSAGE_CONFIRM;
  UpgradeMailbox_Write(mailbox, &request);
  return true;
}
