#ifndef UPGRADE_STORAGE_H
#define UPGRADE_STORAGE_H

#include "upgrade_stage.h"
#include "cmsis_os2.h"

typedef struct
{
  osMessageQueueId_t requests;
  osMessageQueueId_t responses;
  uint32_t protocol_version;
} UpgradeStorageBinding;

typedef struct
{
  uint32_t req_id;
  uint32_t type;
  uint32_t offset;
  uint32_t slot; // 状态槽号 1/A、2/B；元数据操作为 0。
  uint64_t tx_id;
  UpgradeStageResult result;
  int32_t peer_result;
  uint32_t detail;
  uint32_t actual_len;
  bool waiting;
} UpgradeStorageDiagnostic;

/*
 * 在任务上下文绑定请求/应答队列，并接入 UpgradeStage；仅支持持久基线 2。
 * 检查公共协议版本、消息尺寸及可信密钥；句柄和密钥在运行期间有效。
 * 只能绑定一次；依赖未就绪时返回 NOT_READY，不创建队列。
 */
UpgradeStageResult UpgradeStorage_Bind(const UpgradeStorageBinding *binding);
// 普通任务只读快照，不续期、不访问队列，未匹配应答不沿用旧detail。
UpgradeStageResult UpgradeStorage_GetDiagnostic(UpgradeStorageDiagnostic *diagnostic);

#endif
