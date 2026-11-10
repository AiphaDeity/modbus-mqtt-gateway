#ifndef UPGRADE_QUIESCE_H
#define UPGRADE_QUIESCE_H

#include "upgrade_stage.h"

typedef enum
{
  UPGRADE_QUIESCE_MODBUS = 0,
  UPGRADE_QUIESCE_STORAGE,
  UPGRADE_QUIESCE_CONFIG,
  UPGRADE_QUIESCE_NETWORK,
  UPGRADE_QUIESCE_COUNT
} UpgradeQuiesceModule;

typedef struct
{
  UpgradeStageResult (*pause)(void *context, uint32_t timeout_ms);
  UpgradeStageResult (*settle)(void *context, uint32_t timeout_ms);
  UpgradeStageResult (*sync)(void *context, uint32_t timeout_ms);
  UpgradeStageResult (*resume)(void *context, uint32_t timeout_ms);
  void *context;
} UpgradeQuiesceParticipant;

/*
 * 启动时绑定模块的暂停、在途结算、同步和恢复回调，不能与 Run/Resume 并发。
 * 回调必须在 timeout_ms 内返回；无待处理对象可成功，有未保存结果不得结算成功。
 * 仅任务上下文调用；函数及 context 在运行期间有效，绑定不创建任务。
 */
UpgradeStageResult UpgradeQuiesce_Bind(UpgradeQuiesceModule module,
                                       const UpgradeQuiesceParticipant *participant);
// 检查当前构建要求的所有模块是否均已绑定。
bool UpgradeQuiesce_IsReady(void);
// 模块在最终提交/TX前复查该门控；试运行及升级暂停期间拒绝新变更。
bool UpgradeQuiesce_AllowsMutation(void);

// 由会话独占者依次暂停、结算并同步；共用 started_ms 起点和 timeout_ms 总期限。
UpgradeStageResult UpgradeQuiesce_Run(uint32_t started_ms, uint32_t timeout_ms);
// 在 timeout_ms 内逆序恢复已暂停模块；任一失败时保持变更门控关闭。
UpgradeStageResult UpgradeQuiesce_Resume(uint32_t timeout_ms);

#endif
