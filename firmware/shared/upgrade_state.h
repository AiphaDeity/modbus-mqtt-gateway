#ifndef UPGRADE_STATE_H
#define UPGRADE_STATE_H

/*
 * 记录规则和编解码接口不访问存储；CRC 不代表包已认证。
 * 编解码使用 128 B 正文，输入输出不得重叠；失败清零输出，NULL 输出返回 false。
 * Encode 要求 sequence 非零，不写提交标记；Decode 不检查提交标记，启动决策须用 Load。
 */

#include <stdbool.h>
#include <stdint.h>

#include "upgrade_baseline.h"

/*
 * OFFSET 宏为升级元数据区内的字节偏移；物理基址由存储适配映射。
 * 普通日志副本 0/1 各占 4 KiB 扇区，与保存加密包的 A/B 槽不是同一组副本。
 * 每份扇区 +0～127 为状态正文，+128～255 保留，+256～259 最后写独立 CMIT 提交标记。
 * 这里的 128 B 是状态记录，和固件包中同为 128 B 的包头属于不同结构。
 */
#define UPGRADE_STATE_RECORD_SIZE 128U
#define UPGRADE_STATE_COMMIT_OFFSET 256U
#define UPGRADE_STATE_COMMIT_SIZE 4U
#define UPGRADE_STATE_SECTOR_SIZE 4096U
#define UPGRADE_STATE_NORMAL_OFFSET 0U
#define UPGRADE_STATE_RECOVERY_OFFSET 0x2000U
#define UPGRADE_STATE_BARRIER_OFFSET 0x4000U
#define UPGRADE_STATE_TRANSACTION_OFFSET 0x5000U
#define UPGRADE_STATE_METADATA_SIZE 0x7000U
#define UPGRADE_STATE_NORMAL_MAGIC "GWSTATE2"
#define UPGRADE_STATE_RECOVERY_MAGIC "GWRCLOG2"
#define UPGRADE_STATE_NORMAL_COMMIT "CMIT"
#define UPGRADE_STATE_RECOVERY_COMMIT "RCMT"
#define UPGRADE_STATE_SLOT_A_MASK 0x01U
#define UPGRADE_STATE_SLOT_B_MASK 0x02U
#define UPGRADE_STATE_ALL_SLOTS (UPGRADE_STATE_SLOT_A_MASK | UPGRADE_STATE_SLOT_B_MASK)

typedef enum
{
  UPGRADE_STATE_EMPTY = 0,
  UPGRADE_STATE_OK,
  UPGRADE_STATE_IO_ERROR,
  UPGRADE_STATE_CORRUPT,
  UPGRADE_STATE_INVALID,
  UPGRADE_STATE_UNSUPPORTED
} UpgradeStateResult;

typedef enum
{
  UPGRADE_OPERATION_NONE = 0,
  UPGRADE_OPERATION_ENROLL,
  UPGRADE_OPERATION_INSTALL,
  UPGRADE_OPERATION_RESTORE,
  UPGRADE_OPERATION_CANCEL,
  UPGRADE_OPERATION_RECOVERY
} UpgradeStateOperation;

typedef enum
{
  UPGRADE_STATE_CONFIRMED = 1, // 当前受保护包已确认，可作为恢复源。
  UPGRADE_STATE_READY, // 候选包认证及暂停结算完成，尚未擦除片内 App。
  UPGRADE_STATE_INSTALLING, // 安装意图已持久化，复位后可从头重装候选。
  UPGRADE_STATE_TRIAL, // 候选已安装并消耗放行机会，等待 App 健康确认。
  UPGRADE_STATE_RESTORING // 正在从已确认旧包恢复片内 App。
} UpgradeStatePhase;

// slot为0/无包、1/A、2/B；size为完整加密包B数；digest为明文SHA-256。
typedef struct
{
  uint32_t slot;
  uint32_t size;
  uint8_t digest[32];
} UpgradeStatePackage;

// 语义视图，不是Flash存盘结构，不能直接memcpy后落盘。
typedef struct
{
  uint32_t sequence; // 每次日志提交递增的记录序号，非固件版本；加载时用于选择较新副本。
  UpgradeStatePhase phase;
  UpgradeStatePackage confirmed; // 安装/试运行期间保护旧包；确认候选后更新为新的已确认包。
  UpgradeStatePackage candidate; // 新包；确认成功后才提升到 confirmed。
  uint64_t tx_id;             // 稳定事务号，与每次提交递增的sequence不同。
  uint32_t trial_attempts;    // 已提交的放行机会，跳转前递增。
  UpgradeStateOperation operation;
} UpgradeStateRecord;

typedef enum
{
  UPGRADE_RECOVERY_RECEIVING = 1,
  UPGRADE_RECOVERY_READY,
  UPGRADE_RECOVERY_INSTALLING,
  UPGRADE_RECOVERY_TRIAL,
  UPGRADE_RECOVERY_CONFIRMED,
  UPGRADE_RECOVERY_DONE,
  UPGRADE_RECOVERY_BLOCKED
} UpgradeRecoveryPhase;

typedef struct
{
  uint32_t sequence;
  UpgradeRecoveryPhase phase;
  UpgradeStatePackage package;
  uint64_t tx_id;
  uint32_t trial_attempts;
  UpgradeStateOperation operation;
} UpgradeRecoveryRecord;

/*
 * 同步只读回调：offset相对元数据区，范围限定在[0,METADATA_SIZE)，size单位B。
 * 日志读取1、4或128 B；事务首次配置扫描最多256 B，须完整填满data才返回true。
 * 短读、超时或故障返回false，不能以全FF代替读取失败。
 * data只在调用期间有效，返回前结束后台访问；回调自身和整个调用须有界。
 * Boot 独占 SPI；App 由适配转交存储任务，不允许访问元数据区以外的地址。
 */
typedef bool (*UpgradeStateRead)(void *context, uint32_t offset, uint8_t *data, uint32_t size);

// 检查阶段、包角色及事务字段；提交序号由编解码接口检查。
bool UpgradeState_RecordValid(const UpgradeStateRecord *state);
// 比较槽号、包长和摘要是否全部一致。
bool UpgradeState_PackageEqual(const UpgradeStatePackage *a, const UpgradeStatePackage *b);
// 检查普通状态转换及已确认包、候选包的角色变化。
bool UpgradeState_TransitionValid(const UpgradeStateRecord *old, const UpgradeStateRecord *next);
bool UpgradeState_Encode(const UpgradeStateRecord *state, uint32_t sequence, uint8_t *data);
bool UpgradeState_Decode(const uint8_t *data, UpgradeStateRecord *state);
// 检查恢复阶段、包描述及事务字段。
bool UpgradeRecovery_RecordValid(const UpgradeRecoveryRecord *state);
// 检查恢复事务能否从 old 推进到 next。
bool UpgradeRecovery_TransitionValid(const UpgradeRecoveryRecord *old,
                                     const UpgradeRecoveryRecord *next);
bool UpgradeRecovery_Encode(const UpgradeRecoveryRecord *state, uint32_t sequence, uint8_t *data);
bool UpgradeRecovery_Decode(const uint8_t *data, UpgradeRecoveryRecord *state);

/*
 * 读取双副本和另页提交标记，选择最新非回绕序号；state失败清零。
 * 未完整提交的副本可忽略；任一已提交副本损坏、重复序号或读取错误均拒绝回退。
 * 只有两份正文与标记均为 FF 才返回 EMPTY；没有有效副本但存在写入痕迹则返回 CORRUPT。
 * selected可为NULL，成功为副本0/1，失败置0；它不是可写槽号。
 */
UpgradeStateResult UpgradeState_Load(UpgradeStateRead read, void *context,
                                    UpgradeStateRecord *state, uint32_t *selected);
// 原始恢复记录读取，不检查屏障；仅供恢复保存/显式恢复前保护，不能绕过屏障启动。
UpgradeStateResult UpgradeRecovery_LoadRecords(UpgradeStateRead read, void *context,
                                               UpgradeRecoveryRecord *state, uint32_t *selected);
// 屏障首字节为FF才EMPTY；屏障存在但无有效记录是CORRUPT，读失败不是EMPTY。
UpgradeStateResult UpgradeRecovery_Load(UpgradeStateRead read, void *context,
                                        UpgradeRecoveryRecord *state);
// 仅恢复屏障为空时允许普通写入；读取失败时拒绝。
bool UpgradeRecovery_NormalWritesAllowed(UpgradeStateRead read, void *context);

typedef struct
{
  UpgradeStateResult normal_result;
  UpgradeStateResult recovery_result;
  UpgradeStateRecord normal;
  UpgradeRecoveryRecord recovery;
  uint32_t referenced_slots; // 位0/A、位1/B：结构有效记录引用的槽，不是密码认证结果。
  uint32_t writable_slots;   // 仅元数据允许的普通升级候选集合，任何异常/活动事务均为0。
} UpgradeStateProtection;

/*
 * 只读保护快照：普通CONFIRMED且恢复屏障为空时，才给出未引用的候选槽集合。
 * 两边均EMPTY返回EMPTY且writable=0；任一IO_ERROR/CORRUPT关闭写资格并保留诊断。
 * 输出中各自的结果及有效视图可用于定位，不能把整体错误当作空设备。
 * writable不是擦写授权：调用方仍须认证已确认包、匹配本次Boot启动身份并独占
 * 元数据/源包，防止读取后状态变化；快照不代替互斥或候选槽占用。
 */
UpgradeStateResult UpgradeState_ReadProtection(UpgradeStateRead read, void *context,
                                              UpgradeStateProtection *protection);

#endif
