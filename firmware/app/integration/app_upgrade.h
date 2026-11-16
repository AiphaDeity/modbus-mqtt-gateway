#ifndef APP_UPGRADE_H
#define APP_UPGRADE_H

#include "upgrade_mailbox.h"

typedef enum
{
  APP_UPGRADE_WAIT,
  APP_UPGRADE_CONFIRM_RESET,
  APP_UPGRADE_ROLLBACK_RESET
} AppUpgradeAction;

typedef enum
{
  APP_UPGRADE_RESULT_UNKNOWN,
  APP_UPGRADE_RESULT_TRIAL,
  APP_UPGRADE_RESULT_CONFIRMED,
  APP_UPGRADE_RESULT_ROLLED_BACK
} AppUpgradeResult;

// 自检完成后连续健康1s；含90s存储恢复，总预算120s，稳定窗口不叠加到期限外。
#define APP_UPGRADE_STABLE_MS 1000U
#define APP_UPGRADE_DEADLINE_MS 120000U

/*
 * 启动任务在创建业务任务前调用一次，now_ms为单调毫秒时基。
 * boot_managed仅在偏移App下为true；该模式邮箱无效就保持写入门控关闭。
 * 普通独立调试App不读取邮箱；调用后不保留mailbox指针。
 */
void App_UpgradeInit(volatile uint32_t *mailbox, bool boot_managed, uint32_t now_ms);

// 查询本次启动是否处于试运行。
bool App_UpgradeIsTrial(void);
// 控制、配置提交或数据迁移前检查启动门控；通过后仍须业务校验。
bool App_UpgradeCanCommit(void);

// 仅受Boot管理的正常启动返回可信交接身份；普通Debug直启不能用于升级旧源比对。
bool App_UpgradeGetBootIdentity(UpgradeMessage *message);
// 只读本次Boot正常/试运行身份，失败清零；仅用于结果关联，不作为业务提交授权。
bool App_UpgradeGetStartupIdentity(UpgradeMessage *message);
// 现有升级任务每轮调用；成功取得本次启动快照后停止读日志，读失败最多每秒重试一次。
void App_UpgradePollResult(uint32_t now_ms);
// 任务上下文读取本次Boot结果；UNKNOWN不代表失败，TRIAL不代表确认成功。
AppUpgradeResult App_UpgradeGetResult(void);

/*
 * 仅监督任务调用，now_ms 为同一单调毫秒时基；platform_healthy 为平台检查，
 * product_healthy须来自明确的产品自检结果，不能用任务已创建/网络在线代替。
 * 平台与产品同时持续健康1s才请求确认；总计120s未确认则请求复位。
 * 返回RESET后CanCommit仍为false，由监督任务写请求、执行屏障并软件复位。
 */
AppUpgradeAction App_UpgradePoll(uint32_t now_ms, bool platform_healthy, bool product_healthy);
// 确认条件成立后写入匹配本次试运行的邮箱消息；成功后调用方须执行屏障并复位。
bool App_UpgradeWriteConfirmation(volatile uint32_t *mailbox);

#endif
