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

/*
 * 各模块在初始化或任务启动时登记的同步回调，由升级会话独占者调用，业务工作仍由原任务处理。
 * pause 关闭本模块新业务入口；settle 等待已接纳操作及其结果明确结束，不能清队列冒充完成。
 * sync 复核安全点或请求介质同步，不要求每个模块都执行文件 I/O；context 由模块持有。
 * resume 撤销本模块暂停请求；统一变更门控只有所有待恢复模块成功后才重新开放。
 */
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

/*
 * VERIFIED 后、READY 提交前由 Stage 调用；先关统一入口，再依次完成全部 pause、全部 settle、全部 sync。
 * 每轮按 Network→Config→Modbus→Storage 执行，未启用的模块跳过；共用 started_ms 和 timeout_ms 总期限。
 * 成功只表示业务已静默，不写 READY；失败保留门控和已尝试暂停集合，由调用方判断能否恢复。
 */
UpgradeStageResult UpgradeQuiesce_Run(uint32_t started_ms, uint32_t timeout_ms);
/*
 * 在本次恢复的 timeout_ms 总期限内逆序恢复，任一失败仍保持统一变更门控关闭。
 * 调用方须先确认 READY 尚未提交或明确未写；已提交或结果不明时不得用此接口重新开放业务。
 */
UpgradeStageResult UpgradeQuiesce_Resume(uint32_t timeout_ms);

#endif
