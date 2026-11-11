#ifndef UPGRADE_TF_H
#define UPGRADE_TF_H

#include "upgrade_entry.h"

typedef struct
{
  UpgradeStageResult (*open)(void *context, const char *path, uint32_t *size, uint32_t timeout_ms);
  UpgradeStageResult (*read)(void *context, uint32_t offset, uint8_t *data,
                            uint32_t size, uint32_t timeout_ms);
  UpgradeStageResult (*close)(void *context, uint32_t timeout_ms);
  void *context;
} UpgradeTfSource;

typedef struct
{
  uint32_t session_id;
  UpgradeStageResult result;
  UpgradeStageResult close_error;
  UpgradeStageResult abort_error;
} UpgradeTfResult;

/*
 * 在入口开放前绑定一次文件服务；回调同步转交存储任务。
 * open 取得内容不变的文件租约，但不长期持有总线锁；read 必须完整填充缓冲。
 * 缓冲仅在回调期间有效；close 须幂等，关闭失败时禁止复用旧租约。
 * 回调时间单位为 ms，长度/偏移为 B，函数及 context 在运行期间有效。
 */
UpgradeStageResult UpgradeTf_Bind(const UpgradeTfSource *source);
/*
 * 从固定路径导入升级包：先计算摘要，再复制、认证并提交；由升级执行任务调用。
 * 必须显式触发，不在存储、MQTT 或监督任务运行；两遍文件处理各限 180 s。
 * result 分别保留操作、文件关闭及包槽取消的结果，失败不代表资源已释放。
 */
UpgradeStageResult UpgradeTf_Import(const UpgradeEntryPeer *peer, const char *path, UpgradeTfResult *result);
// 只请求取消，不代表文件/包槽已关闭；由导入执行方完成清理并返回结果。
UpgradeStageResult UpgradeTf_RequestCancel(const UpgradeEntryPeer *peer);

#endif
