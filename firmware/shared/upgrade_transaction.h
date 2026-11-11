#ifndef UPGRADE_TRANSACTION_H
#define UPGRADE_TRANSACTION_H

/*
 * 任务/Boot 主线程同步调用，独占元数据；回调不保留缓冲，调用方提供统一总期限。
 * 偏移相对 UPGLOG，擦写限于 0x5000/0x6000 两个扇区。
 */

#include "upgrade_state.h"

typedef struct
{
  UpgradeStateRead read;
  bool (*erase)(void *context, uint32_t offset);
  bool (*program)(void *context, uint32_t offset, const uint8_t *data, uint32_t size);
  void *context;
} UpgradeTransactionStorage;

typedef struct
{
  uint32_t generation;
  uint64_t last_allocated;
} UpgradeTransactionState;

// 读取最新事务号高水位，失败清零输出。
UpgradeStateResult UpgradeTransaction_Load(const UpgradeTransactionStorage *storage,
                                            UpgradeTransactionState *state);

/*
 * 递增高水位并持久提交，读回成功才输出非零 tx_id；失败输出零。
 * 提交结果不明时编号也可能已消耗，禁止回收或降低高水位。
 */
UpgradeStateResult UpgradeTransaction_Reserve(const UpgradeTransactionStorage *storage,
                                               uint64_t *tx_id);

/*
 * 仅显式首次配置调用：完整检查全部升级元数据均FF后才初始化，不擦除残留数据。
 * 不从正常启动、下载或恢复入口自动调用；失败须诊断，不能清空以绕过。
 */
UpgradeStateResult UpgradeTransaction_Initialize(const UpgradeTransactionStorage *storage);

#endif
