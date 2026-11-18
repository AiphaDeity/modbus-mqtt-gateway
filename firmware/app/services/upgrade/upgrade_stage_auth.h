#ifndef UPGRADE_STAGE_AUTH_H
#define UPGRADE_STAGE_AUTH_H

#include "upgrade_stage.h"
#include "upgrade_package.h"

/*
 * 认证候选包并检查向量，成功输出可信 image；失败时 image 不可使用。
 * 由会话独占者同步调用，沿用 finish_started_ms 起点和 Finish 总期限。
 * backend/context 及源包在调用期间有效且不变；存储访问经 backend.read 完成。
 * image.digest 为已认证明文摘要 H_image；本接口不提交持久状态，也不写入片内 App。
 */
UpgradeStageResult UpgradeStage_Authenticate(const UpgradeStageBackend *backend, void *context,
                                            uint32_t session_id, uint32_t total_size,
                                            uint32_t finish_started_ms, UpgradePackageImage *image);

// 调用方已持Stage操作占用；用可信映射验通知摘要32 B和原始P-256签名64 B，不访问介质。
UpgradeStageResult UpgradeStage_AuthenticateNotice(const UpgradeStageBackend *backend, void *context,
                                                  const uint8_t *digest, const uint8_t *signature);

#endif
