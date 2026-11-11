#ifndef BOOT_UPGRADE_STATE_H
#define BOOT_UPGRADE_STATE_H

#include "upgrade_state.h"
#include "upgrade_transaction.h"

// Boot 与 App 共用状态类型及持久编码。
typedef UpgradeStateResult Boot_UpgradeResult;
typedef UpgradeStatePhase Boot_UpgradePhase;
typedef UpgradeStatePackage Boot_UpgradePackage;
typedef UpgradeStateRecord Boot_UpgradeState;

#define BOOT_UPGRADE_EMPTY UPGRADE_STATE_EMPTY
#define BOOT_UPGRADE_OK UPGRADE_STATE_OK
#define BOOT_UPGRADE_IO_ERROR UPGRADE_STATE_IO_ERROR
#define BOOT_UPGRADE_CORRUPT UPGRADE_STATE_CORRUPT
#define BOOT_UPGRADE_INVALID UPGRADE_STATE_INVALID
#define BOOT_UPGRADE_UNSUPPORTED UPGRADE_STATE_UNSUPPORTED
#define BOOT_UPGRADE_CONFIRMED UPGRADE_STATE_CONFIRMED
#define BOOT_UPGRADE_READY UPGRADE_STATE_READY
#define BOOT_UPGRADE_INSTALLING UPGRADE_STATE_INSTALLING
#define BOOT_UPGRADE_TRIAL UPGRADE_STATE_TRIAL
#define BOOT_UPGRADE_RESTORING UPGRADE_STATE_RESTORING

/*
 * 普通上下文同步调用，调用方独占 SPI2；输出失败时清零。
 * EMPTY 仅表示两个记录及提交页均为空白，IO_ERROR 不能当作空白。
 */
Boot_UpgradeResult Boot_UpgradeLoad(Boot_UpgradeState *state);

/*
 * 保存可信调用方提供的状态，自动分配序号；不保存指针，不自动重试。
 * 调用方须先完整认证所引用包；首次登记 confirmed 还须比对当前已确认 App。
 * CRC 仅检测意外损坏，不提供防恶意改写或反降级保护。
 * 返回失败时新记录仍可能已提交，必须重新 Load 后决策，不能沿用旧内存状态。
 */
Boot_UpgradeResult Boot_UpgradeSave(const Boot_UpgradeState *state);

// 包槽擦写入口使用；记录异常拒绝写，已确认包和已提交候选包均受保护。
bool Boot_UpgradeSlotWritable(uint32_t slot);

// 持久预留一个非零事务号，成功才输出；失败不得回收可能已消耗的编号。
Boot_UpgradeResult Boot_UpgradeReserveTransaction(uint64_t *tx_id);
// 显式首次配置事务号，仅全部升级元数据为空白时允许初始化。
Boot_UpgradeResult Boot_UpgradeInitializeTransactions(void);
// 只读加载事务号高水位，失败清零输出。
Boot_UpgradeResult Boot_UpgradeReadTransaction(UpgradeTransactionState *state);

#endif
