#include "app_upgrade.h"
#include "app_modules.h"
#include "upgrade_stage.h"
#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

// 保存 Boot 交接身份；邮箱读后即清，后续核对和确认使用这份启动快照。
static UpgradeMessage boot_message;
static bool initialized;
static bool message_valid;
static bool trial;
static bool stable; // 是否已开始累计本轮连续健康时间，健康中断后重新计时。
static uint32_t started_ms;
static uint32_t stable_since_ms;
static AppUpgradeAction pending_action; // 一旦决定确认或回退，就保持到监督任务执行复位。
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
    // 独立调试没有 Boot 邮箱；这里只允许普通业务启动，不伪造可信升级身份。
    message_valid = true;
  }
  else
  {
    // 先复制到 App 自己的启动快照，再检查 NORMAL、普通 TRIAL 或独立恢复 TRIAL 身份。
    bool received = UpgradeMailbox_Read(mailbox, &boot_message);
    bool trial_message = (boot_message.kind == UPGRADE_MESSAGE_TRIAL)
                      || (boot_message.kind == UPGRADE_MESSAGE_RECOVERY_TRIAL);
    message_valid = received && ((boot_message.kind == UPGRADE_MESSAGE_NORMAL)
                                 || (trial_message && (boot_message.sequence != 0U)));
    trial = message_valid && trial_message;
    // 这里只识别本次启动，尚未确认新版本；连续健康后由 Monitor 推进确认并复位交给 Boot。
    // 消费本次交接，避免旧消息在后续软件复位时被再次当作新确认。
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
    // 恢复试运行使用独立恢复记录，不能套用普通候选槽的结果判定。
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
  // 由升级任务经存储接口查询持久记录；MQTT 只读取已发布结果，不直接访问 Flash。
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

/*
 * Monitor 每轮把平台及产品健康结果传入，决定本次试运行是否需要确认或结束。
 * 本函数不写 Flash、不直接复位；返回动作交给 Monitor 的 ResetForUpgrade 执行。
 * 稳定窗口和总期限均基于本次启动的单调时间，不能用打印启动日志或建任务成功代替健康条件。
 */
AppUpgradeAction App_UpgradePoll(uint32_t now_ms, bool platform_healthy, bool product_healthy)
{
  // 1. 复位动作一旦产生就保持，等待 Monitor 执行，不在后续轮询中撤销或重新累计。
  if (pending_action != APP_UPGRADE_WAIT)
  {
    return pending_action;
  }
  if (!initialized)
  {
    return APP_UPGRADE_WAIT;
  }
  // 2. 无效交接不能发健康确认；有效 NORMAL 启动则无需再次走试运行确认。
  if (!message_valid)
  {
    pending_action = APP_UPGRADE_ROLLBACK_RESET;
    return pending_action;
  }
  if (!trial)
  {
    // 正常已确认启动无需再次生成确认请求，继续由 Monitor 监督业务即可。
    return APP_UPGRADE_WAIT;
  }
  if ((uint32_t)(now_ms - started_ms) >= APP_UPGRADE_DEADLINE_MS)
  {
    // 3. 总试运行期限不会因模块重新报告健康而延长，只请求不带确认的复位。
    // 下一次是否重试或恢复旧包，仍由 Boot 根据次数和实际镜像决定。
    pending_action = APP_UPGRADE_ROLLBACK_RESET;
    return pending_action;
  }
  if (!platform_healthy || !product_healthy)
  {
    // 4. 健康中断只清连续窗口；是否停止喂狗由 Monitor 的平台监督另外决定。
    stable = false;
    return APP_UPGRADE_WAIT;
  }
  if (!stable)
  {
    // 5. 第一次同时满足平台和产品条件时开始 1 s 窗口，不能立即把新版本标为已确认。
    stable = true;
    stable_since_ms = now_ms;
  }
  if ((uint32_t)(now_ms - stable_since_ms) >= APP_UPGRADE_STABLE_MS)
  {
    // 连续健康窗口满足后只提出确认动作；最终写邮箱前仍须原子复查，Boot 复位后才持久确认。
    pending_action = APP_UPGRADE_CONFIRM_RESET;
  }
  return pending_action;
}

/*
 * Monitor 最终复查通过后，将启动快照改成匹配本轮的普通或恢复确认消息，再写入保留邮箱。
 * 不直接把 candidate 改成 confirmed；Boot 在软件复位后匹配身份、重验镜像并保存持久结果。
 */
bool App_UpgradeWriteConfirmation(volatile uint32_t *mailbox)
{
  if ((mailbox == 0) || (pending_action != APP_UPGRADE_CONFIRM_RESET) || !trial)
  {
    return false;
  }
  // 保留事务号、序号、次数和摘要，只把交接消息改为对应的确认消息。
  UpgradeMessage request = boot_message;
  request.kind = (boot_message.kind == UPGRADE_MESSAGE_RECOVERY_TRIAL)
               ? UPGRADE_MESSAGE_RECOVERY_CONFIRM : UPGRADE_MESSAGE_CONFIRM;
  UpgradeMailbox_Write(mailbox, &request);
  return true;
}
