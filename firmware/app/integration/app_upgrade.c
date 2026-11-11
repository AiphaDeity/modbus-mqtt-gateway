#include "app_upgrade.h"

#include <string.h>

static UpgradeMessage boot_message;
static bool initialized;
static bool message_valid;
static bool trial;
static bool stable;
static uint32_t started_ms;
static uint32_t stable_since_ms;
static AppUpgradeAction pending_action;

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
