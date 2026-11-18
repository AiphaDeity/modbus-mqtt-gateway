#ifndef UPGRADE_STAGE_H
#define UPGRADE_STAGE_H

#include <stdbool.h>
#include <stdint.h>
#include "upgrade_state.h"

#define UPGRADE_STAGE_SHA256_SIZE 32U
// Entry/Stage 每次接收的最大字节数，不是 Flash 页大小；队列后端会继续拆成最多 256 B 请求。
#define UPGRADE_STAGE_MAX_CHUNK_SIZE 1016U
// 包开销208 B，明文至少包含8 B向量；App区为448 KiB。
#define UPGRADE_STAGE_MIN_PACKAGE_SIZE 216U
#define UPGRADE_STAGE_MAX_PACKAGE_SIZE (448U * 1024U + 208U)
#define UPGRADE_STAGE_IDLE_TIMEOUT_MS 30000U
#define UPGRADE_STAGE_PREPARE_TIMEOUT_MS 300000U
#define UPGRADE_STAGE_VERIFY_TIMEOUT_MS 180000U
// 暂停、结算、同步及 READY 提交共用的 30 s 总期限，各回调只取得当时剩余时间。
#define UPGRADE_STAGE_COMMIT_TIMEOUT_MS 30000U
// 升级调用时当前 PSP 到栈底的最低可用字节数；历史最低余量另由 Monitor 检查。
#define UPGRADE_EXECUTOR_MIN_FREE_STACK_BYTES 4096U

typedef enum
{
  // 1原为App串口收包，现不再接受；Boot串口恢复使用独立入口。
  UPGRADE_STAGE_SOURCE_NETWORK = 2,
  UPGRADE_STAGE_SOURCE_TF = 3
} UpgradeStageSource;

typedef enum
{
  UPGRADE_STAGE_IDLE, // 无活动接收会话。
  UPGRADE_STAGE_STAGING, // 正在接收候选包，还不能安装。
  UPGRADE_STAGE_VERIFIED, // 包已认证，READY 尚未提交。
  UPGRADE_STAGE_ERROR, // 本次会话失败，仍需检查占用是否清理完成。
  UPGRADE_STAGE_QUIESCING, // 业务暂停及提交处理中，结果不明时也保留此阶段。
  UPGRADE_STAGE_REBOOT_PENDING // READY 已确认持久化，等待系统复位。
} UpgradeStagePhase;

typedef enum
{
  UPGRADE_COMMIT_NOT_WRITTEN, // 明确没有提交，可以按失败路径恢复业务。
  UPGRADE_COMMIT_WRITTEN, // 已完整提交并核对，允许后续复位。
  UPGRADE_COMMIT_UNKNOWN // 可能已提交，仅可读取解析，禁止盲目重写或复用槽。
} UpgradeStageCommitState;

typedef struct
{
  uint32_t slot;
  uint64_t tx_id;
} UpgradeStagePrepared;

typedef enum
{
  UPGRADE_STAGE_OK,
  UPGRADE_STAGE_INVALID,
  UPGRADE_STAGE_MODE,
  UPGRADE_STAGE_BUSY,
  UPGRADE_STAGE_NOT_READY,
  UPGRADE_STAGE_OFFSET,
  UPGRADE_STAGE_INTEGRITY,
  UPGRADE_STAGE_STORAGE,
  UPGRADE_STAGE_AUTH,
  UPGRADE_STAGE_TIMEOUT,
  UPGRADE_STAGE_INTERNAL
} UpgradeStageResult;

typedef struct
{
  UpgradeStagePhase phase;
  UpgradeStageSource source;
  uint32_t session_id;
  uint32_t slot;         // 0未选定、1为A、2为B；由存储适配选择。
  uint64_t tx_id;
  uint32_t total_size;   // 完整加密包字节数。
  uint32_t next_offset;  // 已写入并读回确认的连续字节数。
  UpgradeStageResult error;          // 本会话首次终止原因，直到下一次Begin保留。
  UpgradeStageResult cleanup_error;  // 关闭存储会话的最近一次错误。
  UpgradeStageResult resume_error;   // 恢复业务失败时门控保持关闭，不能由close结果覆盖。
  bool backend_ready;
  bool busy;
  bool cleanup_pending;
  UpgradeStageCommitState commit_state;
} UpgradeStageStatus;

/*
 * 同步后端接口：长度/偏移为 B，超时为 ms；存储操作交给存储任务。
 * 回调须在期限内返回，返回前结束所有缓冲访问和后台修改；服务无法抢占阻塞回调。
 * write/read 的缓冲仅在调用期间有效，超时后结果不明须隔离会话，禁止复用。
 * write 输入可能位于 CCM；后端使用 DMA 时须先复制到 DMA 可访问的缓冲。
 * context 在所有会话期间有效；候选槽占用不应长期持有总线锁。
 */
typedef struct
{
  /*
   * 核对已确认包和恢复屏障，独占可写候选槽并输出槽号/事务号。
   * 失败也可能已取得占用，须通过幂等 close 清理；不得覆盖受保护或状态不明的槽。
   */
  UpgradeStageResult (*prepare)(void *context, uint32_t session_id,
                                uint32_t total_size, UpgradeStagePrepared *prepared,
                                uint32_t timeout_ms);
  // 同步写完本块后返回；实际适配器拆分队列请求，Flash 跨页处理由存储侧驱动完成。
  UpgradeStageResult (*write)(void *context, uint32_t session_id, uint32_t offset,
                              const uint8_t *data, uint32_t size, uint32_t timeout_ms);
  // 按包内字节偏移读取，完整填满输出缓冲才返回 OK。
  UpgradeStageResult (*read)(void *context, uint32_t session_id, uint32_t offset,
                             uint8_t *data, uint32_t size, uint32_t timeout_ms);
  /*
   * 按支持的编号返回可信公钥 65 B/AES 密钥 32 B；指针在整次认证期间有效且不变。
   * 只读查询，不等待外设；缺映射返回 NOT_READY，未知编号返回 AUTH。
   */
  UpgradeStageResult (*keys)(void *context, uint32_t signing_key_id, uint32_t decrypt_key_id,
                             const uint8_t **public_key, const uint8_t **aes_key);
  /*
   * 幂等释放候选槽占用，未打开也返回 OK；不擦包、不提交 READY、不修改已确认记录。
   * 失败须保留占用，供后续 Abort 重试清理。
   */
  UpgradeStageResult (*close)(void *context, uint32_t session_id, uint32_t timeout_ms);
  // 提交失败也须明确是否可能已写入；UNKNOWN后只读解析结果，不重发未知擦写。
  UpgradeStageResult (*commit)(void *context, uint32_t session_id,
                               const UpgradeStatePackage *candidate,
                               bool resolve_only,
                               UpgradeStageCommitState *state, uint32_t timeout_ms);
  // 只读持久状态，与收包共用工作区；不得擦写或改变包槽占用。
  UpgradeStageResult (*read_boot_state)(void *context, UpgradeStateProtection *state,
                                         uint32_t timeout_ms);
} UpgradeStageBackend;

/*
 * 启动任务初始化接收状态；重复调用保留状态，不执行 I/O。
 * 本模块接口仅用于调度器运行后的任务上下文；并发修改立即返回 BUSY。
 */
bool UpgradeStage_Init(void);
// 检查当前任务是否适合执行升级：排除指定业务/存储/监督任务，并检查当前栈空间。
bool UpgradeStage_ExecutorAllowed(void);

// 启动装配时绑定完整后端一次；复制函数表，不保留 backend 指针，context 可为 NULL。
UpgradeStageResult UpgradeStage_Bind(const UpgradeStageBackend *backend, void *context);
// 仅升级执行任务在空闲或已清理ERROR阶段读取；timeout_ms为1～3000 ms，失败清零输出。
UpgradeStageResult UpgradeStage_ReadBootState(UpgradeStateProtection *state, uint32_t timeout_ms);

// 升级执行任务在空闲时验通知摘要32 B/原始签名64 B，复用操作锁及可信映射；不擦写或分配事务号。
UpgradeStageResult UpgradeStage_VerifyNotice(const uint8_t *digest, const uint8_t *signature);

/*
 * Begin成功才输出非零会话号，失败输出0；package_sha256可读32 B，不保留指针。
 * 外部调用方通过UpgradeEntry校验权限并绑定本次下载/TF操作；source不是授权凭证。
 * 会话号本次启动内不复用，重启不续传；新下载尝试不可沿用旧尝试的会话号。
 * 未绑定依赖、试运行或无提交资格时拒绝；ERROR可在关闭旧占用后重新开始。
 */
UpgradeStageResult UpgradeStage_Begin(UpgradeStageSource source, uint32_t total_size,
                                     const uint8_t *package_sha256, uint32_t *session_id);

/*
 * Write同步读取1～1016 B输入，data在返回前须可读且不变，返回后可复用；不保留指针。
 * 只接受连续追加或完全位于已确认区间、内容相同的重复块；读回确认后才推进。
 * 重复块、查询、错误请求均不延长30 s无进展期限，跨HAL tick回绕仍有效。
 */
UpgradeStageResult UpgradeStage_Write(uint32_t session_id, uint32_t offset,
                                     const uint8_t *data, uint32_t size);

/*
 * 读回整包检查传输摘要，并完成认证和向量检查；共用 180 s 固定期限。
 * 由升级执行任务同步调用；OK 仅到 VERIFIED，仍占用候选包，不提交 READY 或重启。
 * 同会话重复 Finish 幂等；需取消时显式调用 Abort。
 */
UpgradeStageResult UpgradeStage_Finish(uint32_t session_id);
// 取消未提交会话并关闭存储占用；提交不明或 READY 已持久化时拒绝。
UpgradeStageResult UpgradeStage_Abort(uint32_t session_id);
// 其他维护任务可请求取消接收/认证；只置标记，不访问存储，不代表占用已释放。
UpgradeStageResult UpgradeStage_RequestAbort(uint32_t session_id);
// 查询匹配的 STAGING/VERIFIED 会话是否已请求取消；其他情况返回 false。
bool UpgradeStage_CancellationRequested(uint32_t session_id);

// VERIFIED后执行暂停/结算/同步及持久READY；成功转REBOOT_PENDING，不直接跳App。
UpgradeStageResult UpgradeStage_Commit(uint32_t session_id);
// 仅READY已确认持久时允许；传输方发完成功响应后调用，Poll另提供1s重启兜底。
UpgradeStageResult UpgradeStage_Reboot(uint32_t session_id);

/*
 * 监督任务周期调用：标记接收超时，并在 READY 持久化 1 s 后执行复位。
 * 不访问存储；过期会话由后续修改操作关闭，关闭失败时继续保留占用。
 */
void UpgradeStage_Poll(void);
// 获取一致的 RAM 快照并检查到期；不更新进展时间，不执行 I/O 或重启。
UpgradeStageResult UpgradeStage_GetStatus(UpgradeStageStatus *status);

#endif
