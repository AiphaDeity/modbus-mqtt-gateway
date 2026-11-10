#ifndef UPGRADE_ENTRY_H
#define UPGRADE_ENTRY_H

#include "upgrade_stage.h"

#define UPGRADE_TF_PATH "/firmware.gwp"

// epoch由调用方为每次下载尝试/TF导入分配，非零且本次启动不复用；不是云端任务号。
typedef struct
{
  UpgradeStageSource source;
  uint32_t epoch;
} UpgradeEntryPeer;

/*
 * 按 source/epoch 查询当前操作权限；回调须只读、无 I/O，并支持并发查询。
 * 回调及 context 在运行期间有效；撤权时还须用原 peer 调用 Disconnect。
 */
typedef struct
{
  bool (*authorize)(void *context, const UpgradeEntryPeer *peer);
  void *context;
} UpgradeEntryPolicy;

// 启动装配时绑定一次权限策略；未绑定时拒绝接收升级包。
UpgradeStageResult UpgradeEntry_BindPolicy(const UpgradeEntryPolicy *policy);
// 返回错误码或阶段的静态名称，供协议响应和日志使用。
const char *UpgradeEntry_ErrorName(UpgradeStageResult result);
const char *UpgradeEntry_PhaseName(UpgradeStagePhase phase);

/*
 * 建立下载会话并准备候选槽；仅升级执行任务调用，不得占用 MQTT、网络、存储或监督任务。
 * peer 在同一次下载/导入期间不变；TF 使用固定路径，网络来源的 path 必须为 NULL。
 * total_size 为完整包字节数，sha256 指向 32 B 摘要；成功才输出非零 session_id。
 * 准备失败若未能清理占用，仍保留原 peer 的会话归属。
 */
UpgradeStageResult UpgradeEntry_Begin(const UpgradeEntryPeer *peer, const char *path,
                                      uint32_t total_size, const uint8_t *sha256, uint32_t *session_id);
/*
 * 按字节偏移写入 1～1016 B 数据，返回后可复用 data；由升级执行任务同步调用。
 * peer/session_id 必须匹配当前会话；连续 30 s 无新增读回数据则超时。
 */
UpgradeStageResult UpgradeEntry_Write(const UpgradeEntryPeer *peer, uint32_t session_id,
                                      uint32_t offset, const uint8_t *data, uint32_t size);
/*
 * 校验候选包，暂停业务并提交 READY；由升级执行任务同步调用。
 * OK 表示 READY 已持久化，将在 1 s 后重启，安装及健康结果须重启后确认。
 * 提交结果不明时保持暂停，重复调用仅核对提交结果。
 */
UpgradeStageResult UpgradeEntry_Finish(const UpgradeEntryPeer *peer, uint32_t session_id);
// 取消当前 peer/session_id 的会话并清理占用；READY 或提交不明时拒绝取消。
UpgradeStageResult UpgradeEntry_Abort(const UpgradeEntryPeer *peer, uint32_t session_id);
// 下载中断/撤权时用原epoch通知，不要求authorize仍为true；BUSY须稍后结算，提交可能拒绝取消。
UpgradeStageResult UpgradeEntry_Disconnect(const UpgradeEntryPeer *peer);
// 当前会话 READY 已持久化后执行软件复位；成功时不返回。
UpgradeStageResult UpgradeEntry_Reboot(const UpgradeEntryPeer *peer, uint32_t session_id);
// 校验 peer 当前权限后读取状态快照；不访问存储，不触发重启。
UpgradeStageResult UpgradeEntry_GetStatus(const UpgradeEntryPeer *peer, UpgradeStageStatus *status);

#endif
