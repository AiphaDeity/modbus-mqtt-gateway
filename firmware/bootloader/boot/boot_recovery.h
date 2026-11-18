#ifndef BOOT_RECOVERY_H
#define BOOT_RECOVERY_H

#include "boot_install.h"
#include "boot_upgrade_state.h"

typedef enum
{
  BOOT_RECEIVE_NO_REQUEST,
  BOOT_RECEIVE_COMMAND,
  BOOT_RECEIVE_PACKAGE
} BootReceiveResult;

// 升级日志区内另用12KiB：两份4KiB恢复记录和一个4KiB启动屏障，不与普通日志重叠。
#define BOOT_RECOVERY_LOG_OFFSET UPGRADE_STATE_RECOVERY_OFFSET
#define BOOT_RECOVERY_BARRIER_OFFSET UPGRADE_STATE_BARRIER_OFFSET

typedef UpgradeRecoveryPhase Boot_RecoveryPhase;
typedef UpgradeRecoveryRecord Boot_RecoveryState;

#define BOOT_RECOVERY_RECEIVING UPGRADE_RECOVERY_RECEIVING
#define BOOT_RECOVERY_READY UPGRADE_RECOVERY_READY
#define BOOT_RECOVERY_INSTALLING UPGRADE_RECOVERY_INSTALLING
#define BOOT_RECOVERY_TRIAL UPGRADE_RECOVERY_TRIAL
#define BOOT_RECOVERY_CONFIRMED UPGRADE_RECOVERY_CONFIRMED
#define BOOT_RECOVERY_DONE UPGRADE_RECOVERY_DONE
#define BOOT_RECOVERY_BLOCKED UPGRADE_RECOVERY_BLOCKED

/*
 * 同步普通上下文，调用方独占SPI3，输出失败清零。
 * EMPTY只表示启动屏障为空白；屏障已写而无有效记录必须留恢复，不回退普通启动。
 */
Boot_UpgradeResult Boot_RecoveryLoad(Boot_RecoveryState *state);
/*
 * 保存合法恢复转换并读回核对；返回失败仍可能已提交，不自动重发写操作。
 * const 入参不更新新序号，需要最新身份或决定后续处理前，须重新调用 Boot_RecoveryLoad。
 */
Boot_UpgradeResult Boot_RecoverySave(const Boot_RecoveryState *state);

/*
 * 显式恢复请求：slot为1(A)/2(B)，package_size为整包字节数。
 * 先保护可信confirmed槽、提交恢复屏障和RECEIVING，再允许擦写选定槽。
 * allow_unknown只允许绕过损坏元数据，不能绕过读取失败或可信旧包保护。
 * 失败可能已进入恢复模式，不能据此启动App；不在此函数擦包槽。
 */
Boot_UpgradeResult Boot_RecoveryBegin(uint32_t slot, uint32_t package_size, bool allow_unknown);
// 只认证接收到的包并持久登记READY；OK不表示App已安装，不允许据此跳转。
Boot_UpgradeResult Boot_RecoveryAccept(const uint8_t *public_key, const uint8_t *aes_key);
// 推进恢复安装、试运行或健康确认；无可信恢复源时保持 Boot，不自动放行 App。
Boot_InstallResult Boot_RecoveryRun(const uint8_t *public_key, const uint8_t *aes_key);

// 底层包槽写入口使用：普通写仅屏障为空时允许，恢复写仅允许RECEIVING引用的槽。
bool Boot_RecoveryNormalWritesAllowed(void);
bool Boot_RecoveryWriteAllowed(uint32_t address, uint32_t size);

/*
 * Boot 主线程同步处理维护帧，独占 USART2；使用 Boot 自己的 SPI3 和包认证接口，不依赖 App。
 * 当前维护口 PD5/TX、PD6/RX，115200/8N1；USART1 单独输出日志，维护串口由电脑工具独占。
 * keys 调用期间有效，NULL 时仍可查询，但不能建立新的恢复收包会话。
 * BOOT_RECEIVE_PACKAGE 表示已认证并持久 READY，main 才能安装；其他返回值不表示恢复完成。
 */
BootReceiveResult Boot_RecoveryReceive(const uint8_t *public_key, const uint8_t *aes_key);
// 回应最近 FINISH 的第二条安装阶段结果；须先收到 PACKAGE 并运行安装器，TRIAL 不代表健康确认。
void Boot_RecoveryReplyResult(Boot_InstallResult result);

#endif
