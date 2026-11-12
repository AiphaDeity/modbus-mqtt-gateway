#include "upgrade_stage.h"
#include "upgrade_stage_auth.h"
#include "upgrade_quiesce.h"
#include "upgrade_mailbox.h"

#include "app_upgrade.h"
#include "app_modules.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"
#include "mbedtls/sha256.h"

#include <string.h>

static bool initialized;
static bool operation_busy;
static bool backend_ready;
static bool storage_open;
// 仅针对已发布会话；Entry另行锁存连接断线，覆盖尚无会话号的准备期。
static bool cancel_requested;
static uint32_t allocated_session_id;
static uint32_t last_progress_ms;
static uint32_t commit_started_ms;
static uint32_t reboot_pending_ms;
static UpgradePackageImage verified_image;
static uint8_t expected_sha256[UPGRADE_STAGE_SHA256_SIZE];
static UpgradeStageBackend backend;
static void *backend_context;
// session仅由占有operation_busy的任务修改；查询只读取临界区内发布的快照。
static UpgradeStageStatus session;
static UpgradeStageStatus published_status;

// 普通SRAM固定工作区，不将整包放RAM；同一时间只有一个修改操作使用。
static uint8_t readback[256] __attribute__((aligned(8)));

// 检查当前是否处于调度器运行后的任务上下文。
static bool TaskContext(void)
{
  return (__get_IPSR() == 0U) && (osKernelGetState() == osKernelRunning);
}

// 排除业务和监督任务，并检查当前任务的历史最小空闲栈。
bool UpgradeStage_ExecutorAllowed(void)
{
  return TaskContext() && !App_IsCurrentModuleTask(APP_MODULE_STORAGE)
      && !App_IsCurrentModuleTask(APP_MODULE_MQTT) && !App_IsCurrentModuleTask(APP_MODULE_MODBUS)
      && !App_IsCurrentModuleTask(APP_MODULE_MONITOR)
      && ((uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)
          >= UPGRADE_EXECUTOR_MIN_FREE_STACK_BYTES);
}

// 长操作期间不关闭中断；其他修改请求返回BUSY，查询仍可读取上次完整快照。
static UpgradeStageResult TakeOperation(void)
{
  if (!TaskContext())
  {
    return UPGRADE_STAGE_MODE;
  }
  UpgradeStageResult result = UPGRADE_STAGE_OK;
  taskENTER_CRITICAL();
  if (!initialized)
  {
    result = UPGRADE_STAGE_NOT_READY;
  }
  else if (operation_busy)
  {
    result = UPGRADE_STAGE_BUSY;
  }
  else
  {
    operation_busy = true;
  }
  taskEXIT_CRITICAL();
  return result;
}

// 在临界区发布完整会话快照，供并发查询使用。
static void PublishStatus(void)
{
  session.backend_ready = backend_ready;
  session.cleanup_pending = storage_open && (session.phase == UPGRADE_STAGE_ERROR)
                         && (session.commit_state != UPGRADE_COMMIT_UNKNOWN);
  taskENTER_CRITICAL();
  published_status = session;
  taskEXIT_CRITICAL();
}

// 发布最新状态并释放修改操作占用。
static UpgradeStageResult ReleaseOperation(UpgradeStageResult result)
{
  PublishStatus();
  taskENTER_CRITICAL();
  operation_busy = false;
  taskEXIT_CRITICAL();
  return result;
}

// 起点由本服务持有，不接受传输方提供的时间；无符号减法处理32位回绕。
static uint32_t RemainingMs(uint32_t started_ms, uint32_t timeout_ms)
{
  uint32_t elapsed_ms = (uint32_t)(HAL_GetTick() - started_ms);
  return (elapsed_ms < timeout_ms) ? timeout_ms - elapsed_ms : 0U;
}

// 记录本次会话的首个错误，后续清理错误另行保存。
static void SetError(UpgradeStageResult error)
{
  session.phase = UPGRADE_STAGE_ERROR;
  if (session.error == UPGRADE_STAGE_OK)
  {
    session.error = error;
  }
}

// 检查接收是否超过无进展期限。
static void CheckIdleTimeout(void)
{
  if (UpgradeStage_CancellationRequested(session.session_id))
  {
    SetError(UPGRADE_STAGE_MODE);
  }
  if ((session.phase == UPGRADE_STAGE_STAGING)
      && (RemainingMs(last_progress_ms, UPGRADE_STAGE_IDLE_TIMEOUT_MS) == 0U))
  {
    SetError(UPGRADE_STAGE_TIMEOUT);
  }
}

// 只在修改请求的调用任务中执行关闭，监督任务不等待存储。
static UpgradeStageResult CloseStorage(void)
{
  if (!storage_open)
  {
    return UPGRADE_STAGE_OK;
  }
  uint32_t started_ms = HAL_GetTick();
  UpgradeStageResult result = backend.close(backend_context, session.session_id,
                                            UPGRADE_STAGE_IDLE_TIMEOUT_MS);
  if (RemainingMs(started_ms, UPGRADE_STAGE_IDLE_TIMEOUT_MS) == 0U)
  {
    result = UPGRADE_STAGE_TIMEOUT;
  }
  session.cleanup_error = result;
  if (result == UPGRADE_STAGE_OK)
  {
    storage_open = false;
  }
  return result;
}

// 记录会话失败并尝试关闭存储，清理失败时保留占用。
static UpgradeStageResult FailSession(UpgradeStageResult error)
{
  SetError(error);
  PublishStatus();
  // 关闭错误单独保留，不覆盖第一次失败；关闭未确认前不允许新会话复用槽。
  session.cleanup_error = CloseStorage();
  return session.error;
}

// 核对会话号、阶段及接收期限。
static UpgradeStageResult CheckSession(uint32_t session_id)
{
  if ((session_id == 0U) || (session_id != session.session_id)
      || (session.phase == UPGRADE_STAGE_IDLE))
  {
    return UPGRADE_STAGE_MODE;
  }
  CheckIdleTimeout();
  if (session.phase == UPGRADE_STAGE_ERROR)
  {
    return FailSession(session.error);
  }
  return UPGRADE_STAGE_OK;
}

// 比较整块的持久读回；追加和重复块共用，不靠CRC或最后一块缓存判断相同。
static UpgradeStageResult ReadMatches(uint32_t offset, const uint8_t *expected, uint32_t size)
{
  uint32_t compared = 0U;
  while (compared < size)
  {
    uint32_t count = size - compared;
    if (count > sizeof(readback))
    {
      count = sizeof(readback);
    }
    uint32_t remaining = RemainingMs(last_progress_ms, UPGRADE_STAGE_IDLE_TIMEOUT_MS);
    if (remaining == 0U)
    {
      return UPGRADE_STAGE_TIMEOUT;
    }
    UpgradeStageResult result = backend.read(backend_context, session.session_id,
                                             offset + compared, readback, count, remaining);
    if (RemainingMs(last_progress_ms, UPGRADE_STAGE_IDLE_TIMEOUT_MS) == 0U)
    {
      return UPGRADE_STAGE_TIMEOUT;
    }
    if (result != UPGRADE_STAGE_OK)
    {
      return result;
    }
    if (memcmp(readback, expected + compared, count) != 0)
    {
      return UPGRADE_STAGE_INTEGRITY;
    }
    compared += count;
  }
  return UPGRADE_STAGE_OK;
}

// 初始化接收状态和取消标记，重复调用不重置会话。
bool UpgradeStage_Init(void)
{
  if (!TaskContext())
  {
    return false;
  }
  taskENTER_CRITICAL();
  if (!initialized)
  {
    session.phase = UPGRADE_STAGE_IDLE;
    published_status = session;
    initialized = true;
  }
  taskEXIT_CRITICAL();
  return true;
}

// 校验后端回调并复制绑定，不创建存储资源。
UpgradeStageResult UpgradeStage_Bind(const UpgradeStageBackend *adapter, void *context)
{
  if ((adapter == NULL) || (adapter->prepare == NULL) || (adapter->write == NULL)
      || (adapter->read == NULL) || (adapter->keys == NULL) || (adapter->close == NULL)
      || (adapter->commit == NULL) || (adapter->read_boot_state == NULL))
  {
    return UPGRADE_STAGE_INVALID;
  }
  UpgradeStageResult result = TakeOperation();
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (backend_ready)
  {
    return ReleaseOperation(UPGRADE_STAGE_BUSY);
  }
  backend = *adapter;
  backend_context = context;
  backend_ready = true;
  return ReleaseOperation(UPGRADE_STAGE_OK);
}

// 与收包共用修改占用，避免只读查询抢用存储请求和应答工作区。
UpgradeStageResult UpgradeStage_ReadBootState(UpgradeStateProtection *state, uint32_t timeout_ms)
{
  if (state == NULL)
  {
    return UPGRADE_STAGE_INVALID;
  }
  memset(state, 0, sizeof(*state));
  if ((timeout_ms == 0U) || (timeout_ms > 3000U))
  {
    return UPGRADE_STAGE_INVALID;
  }
  if (!UpgradeStage_ExecutorAllowed())
  {
    return UPGRADE_STAGE_MODE;
  }
  UpgradeStageResult result = TakeOperation();
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (!backend_ready)
  {
    return ReleaseOperation(UPGRADE_STAGE_NOT_READY);
  }
  if (storage_open || (session.resume_error != UPGRADE_STAGE_OK)
      || ((session.phase != UPGRADE_STAGE_IDLE) && (session.phase != UPGRADE_STAGE_ERROR)))
  {
    return ReleaseOperation(UPGRADE_STAGE_BUSY);
  }
  uint32_t started_ms = HAL_GetTick();
  result = backend.read_boot_state(backend_context, state, timeout_ms);
  if (RemainingMs(started_ms, timeout_ms) == 0U)
  {
    result = UPGRADE_STAGE_TIMEOUT;
  }
  if (result != UPGRADE_STAGE_OK)
  {
    memset(state, 0, sizeof(*state));
  }
  return ReleaseOperation(result);
}

// 检查准入和旧占用，分配会话并准备候选槽。
UpgradeStageResult UpgradeStage_Begin(UpgradeStageSource source, uint32_t total_size,
                                     const uint8_t *package_sha256, uint32_t *session_id)
{
  if (session_id == NULL)
  {
    return UPGRADE_STAGE_INVALID;
  }
  *session_id = 0U;
  if ((package_sha256 == NULL)
      || ((source != UPGRADE_STAGE_SOURCE_NETWORK) && (source != UPGRADE_STAGE_SOURCE_TF))
      || (total_size < UPGRADE_STAGE_MIN_PACKAGE_SIZE)
      || (total_size > UPGRADE_STAGE_MAX_PACKAGE_SIZE))
  {
    return UPGRADE_STAGE_INVALID;
  }
  UpgradeStageResult result = TakeOperation();
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (!backend_ready)
  {
    return ReleaseOperation(UPGRADE_STAGE_NOT_READY);
  }
  if (!UpgradeStage_ExecutorAllowed())
  {
    return ReleaseOperation(UPGRADE_STAGE_MODE);
  }
  if (!UpgradeQuiesce_AllowsMutation())
  {
    return ReleaseOperation(UPGRADE_STAGE_MODE);
  }
  CheckIdleTimeout();
  if ((session.phase == UPGRADE_STAGE_STAGING) || (session.phase == UPGRADE_STAGE_VERIFIED)
      || (session.phase == UPGRADE_STAGE_QUIESCING) || (session.phase == UPGRADE_STAGE_REBOOT_PENDING))
  {
    return ReleaseOperation(UPGRADE_STAGE_BUSY);
  }
  result = CloseStorage();
  if (result != UPGRADE_STAGE_OK)
  {
    return ReleaseOperation(result);
  }
  if (allocated_session_id == UINT32_MAX)
  {
    return ReleaseOperation(UPGRADE_STAGE_MODE);
  }

  memset(&session, 0, sizeof(session));
  taskENTER_CRITICAL();
  cancel_requested = false;
  taskEXIT_CRITICAL();
  session.source = source;
  session.session_id = ++allocated_session_id;
  session.total_size = total_size;
  session.phase = UPGRADE_STAGE_STAGING;
  memcpy(expected_sha256, package_sha256, sizeof(expected_sha256));
  memset(&verified_image, 0, sizeof(verified_image));
  PublishStatus();

  // prepare失败也可能已取得占用；close必须能幂等释放这个会话。
  storage_open = true;
  uint32_t started_ms = HAL_GetTick();
  UpgradeStagePrepared prepared = {0};
  result = backend.prepare(backend_context, session.session_id, total_size, &prepared,
                            UPGRADE_STAGE_PREPARE_TIMEOUT_MS);
  session.slot = prepared.slot;
  session.tx_id = prepared.tx_id;
  if (UpgradeStage_CancellationRequested(session.session_id))
  {
    result = UPGRADE_STAGE_MODE;
  }
  if ((result == UPGRADE_STAGE_OK) && (session.slot != 1U) && (session.slot != 2U))
  {
    result = UPGRADE_STAGE_INTERNAL;
  }
  if ((result == UPGRADE_STAGE_OK) && (session.tx_id == 0U))
  {
    result = UPGRADE_STAGE_INTERNAL;
  }
  uint32_t completed_ms = HAL_GetTick();
  if ((uint32_t)(completed_ms - started_ms) >= UPGRADE_STAGE_PREPARE_TIMEOUT_MS)
  {
    result = UPGRADE_STAGE_TIMEOUT;
  }
  if (result != UPGRADE_STAGE_OK)
  {
    return ReleaseOperation(FailSession(result));
  }
  last_progress_ms = completed_ms;
  *session_id = session.session_id;
  return ReleaseOperation(UPGRADE_STAGE_OK);
}

// 写入连续块或核对重复块，持久读回一致后才推进偏移。
UpgradeStageResult UpgradeStage_Write(uint32_t session_id, uint32_t offset,
                                     const uint8_t *data, uint32_t size)
{
  if ((data == NULL) || (size == 0U) || (size > UPGRADE_STAGE_MAX_CHUNK_SIZE))
  {
    return UPGRADE_STAGE_INVALID;
  }
  if (!UpgradeStage_ExecutorAllowed())
  {
    return UPGRADE_STAGE_MODE;
  }
  UpgradeStageResult result = TakeOperation();
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  result = CheckSession(session_id);
  if (result != UPGRADE_STAGE_OK)
  {
    return ReleaseOperation(result);
  }
  if (session.phase != UPGRADE_STAGE_STAGING)
  {
    return ReleaseOperation(UPGRADE_STAGE_MODE);
  }
  if ((offset > session.next_offset) || (size > session.total_size - offset)
      || ((offset < session.next_offset) && (size > session.next_offset - offset)))
  {
    return ReleaseOperation(UPGRADE_STAGE_OFFSET);
  }
  bool append = (offset == session.next_offset);
  if (append)
  {
    uint32_t remaining = RemainingMs(last_progress_ms, UPGRADE_STAGE_IDLE_TIMEOUT_MS);
    if (remaining == 0U)
    {
      return ReleaseOperation(FailSession(UPGRADE_STAGE_TIMEOUT));
    }
    result = backend.write(backend_context, session_id, offset, data, size, remaining);
    if (RemainingMs(last_progress_ms, UPGRADE_STAGE_IDLE_TIMEOUT_MS) == 0U)
    {
      result = UPGRADE_STAGE_TIMEOUT;
    }
  }
  if (result == UPGRADE_STAGE_OK)
  {
    result = ReadMatches(offset, data, size);
  }
  uint32_t completed_ms = HAL_GetTick();
  if (UpgradeStage_CancellationRequested(session_id))
  {
    result = UPGRADE_STAGE_MODE;
  }
  if ((uint32_t)(completed_ms - last_progress_ms) >= UPGRADE_STAGE_IDLE_TIMEOUT_MS)
  {
    result = UPGRADE_STAGE_TIMEOUT;
  }
  if (result != UPGRADE_STAGE_OK)
  {
    return ReleaseOperation(FailSession(result));
  }
  if (append)
  {
    session.next_offset += size;
    last_progress_ms = completed_ms;
  }
  return ReleaseOperation(UPGRADE_STAGE_OK);
}

// 从实际存储重新读取整包，传输摘要覆盖签名字节，与Boot的验签摘要范围不同。
static UpgradeStageResult CheckPackageHash(uint32_t started_ms)
{
  mbedtls_sha256_context hash;
  uint8_t digest[UPGRADE_STAGE_SHA256_SIZE];
  mbedtls_sha256_init(&hash);
  UpgradeStageResult result = UPGRADE_STAGE_OK;
  if (mbedtls_sha256_starts(&hash, 0) != 0)
  {
    result = UPGRADE_STAGE_INTERNAL;
  }
  uint32_t offset = 0U;
  while ((result == UPGRADE_STAGE_OK) && (offset < session.total_size))
  {
    if (UpgradeStage_CancellationRequested(session.session_id))
    {
      result = UPGRADE_STAGE_MODE;
      break;
    }
    uint32_t count = session.total_size - offset;
    if (count > sizeof(readback))
    {
      count = sizeof(readback);
    }
    uint32_t remaining = RemainingMs(started_ms, UPGRADE_STAGE_VERIFY_TIMEOUT_MS);
    if (remaining == 0U)
    {
      result = UPGRADE_STAGE_TIMEOUT;
      break;
    }
    result = backend.read(backend_context, session.session_id, offset, readback, count, remaining);
    if (RemainingMs(started_ms, UPGRADE_STAGE_VERIFY_TIMEOUT_MS) == 0U)
    {
      result = UPGRADE_STAGE_TIMEOUT;
    }
    if ((result == UPGRADE_STAGE_OK) && (mbedtls_sha256_update(&hash, readback, count) != 0))
    {
      result = UPGRADE_STAGE_INTERNAL;
    }
    offset += count;
  }
  if (result == UPGRADE_STAGE_OK)
  {
    if (mbedtls_sha256_finish(&hash, digest) != 0)
    {
      result = UPGRADE_STAGE_INTERNAL;
    }
    else if (memcmp(digest, expected_sha256, sizeof(digest)) != 0)
    {
      result = UPGRADE_STAGE_INTEGRITY;
    }
  }
  mbedtls_sha256_free(&hash);
  return result;
}

// 核对完整传输摘要并认证候选包，成功转为 VERIFIED。
UpgradeStageResult UpgradeStage_Finish(uint32_t session_id)
{
  if (!UpgradeStage_ExecutorAllowed())
  {
    return UPGRADE_STAGE_MODE;
  }
  UpgradeStageResult result = TakeOperation();
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  result = CheckSession(session_id);
  if ((result != UPGRADE_STAGE_OK) || (session.phase == UPGRADE_STAGE_VERIFIED))
  {
    return ReleaseOperation(result);
  }
  if (session.phase != UPGRADE_STAGE_STAGING)
  {
    return ReleaseOperation(UPGRADE_STAGE_BUSY);
  }
  if (session.next_offset != session.total_size)
  {
    return ReleaseOperation(UPGRADE_STAGE_OFFSET);
  }
  uint32_t started_ms = HAL_GetTick();
  result = CheckPackageHash(started_ms);
  uint32_t remaining = RemainingMs(started_ms, UPGRADE_STAGE_VERIFY_TIMEOUT_MS);
  if (remaining == 0U)
  {
    result = UPGRADE_STAGE_TIMEOUT;
  }
  if (result == UPGRADE_STAGE_OK)
  {
    result = UpgradeStage_Authenticate(&backend, backend_context, session_id,
                                        session.total_size, started_ms, &verified_image);
    if (RemainingMs(started_ms, UPGRADE_STAGE_VERIFY_TIMEOUT_MS) == 0U)
    {
      result = UPGRADE_STAGE_TIMEOUT;
    }
  }
  if (result != UPGRADE_STAGE_OK)
  {
    return ReleaseOperation(FailSession(result));
  }
  session.phase = UPGRADE_STAGE_VERIFIED;
  return ReleaseOperation(UPGRADE_STAGE_OK);
}

// 为匹配且可取消的会话置位取消标记。
UpgradeStageResult UpgradeStage_RequestAbort(uint32_t session_id)
{
  if (!TaskContext())
  {
    return UPGRADE_STAGE_MODE;
  }
  taskENTER_CRITICAL();
  UpgradeStageResult result = UPGRADE_STAGE_MODE;
  if (initialized && (session_id != 0U) && (published_status.session_id == session_id))
  {
    if ((published_status.phase == UPGRADE_STAGE_STAGING)
        || (published_status.phase == UPGRADE_STAGE_VERIFIED))
    {
      cancel_requested = true;
      result = UPGRADE_STAGE_OK;
    }
    else if ((published_status.phase == UPGRADE_STAGE_QUIESCING)
             || (published_status.phase == UPGRADE_STAGE_REBOOT_PENDING))
    {
      result = UPGRADE_STAGE_BUSY;
    }
  }
  taskEXIT_CRITICAL();
  return result;
}

// 查询匹配的接收或已认证会话是否已请求取消。
bool UpgradeStage_CancellationRequested(uint32_t session_id)
{
  taskENTER_CRITICAL();
  bool requested = cancel_requested && (published_status.session_id == session_id)
                && ((published_status.phase == UPGRADE_STAGE_STAGING)
                    || (published_status.phase == UPGRADE_STAGE_VERIFIED));
  taskEXIT_CRITICAL();
  return requested;
}

// 取消未提交会话并关闭后端，保留清理失败信息。
UpgradeStageResult UpgradeStage_Abort(uint32_t session_id)
{
  if (!UpgradeStage_ExecutorAllowed())
  {
    return UPGRADE_STAGE_MODE;
  }
  UpgradeStageResult result = TakeOperation();
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if ((session_id == 0U) || (session_id != session.session_id))
  {
    return ReleaseOperation(UPGRADE_STAGE_MODE);
  }
  if ((session.phase == UPGRADE_STAGE_QUIESCING) || (session.phase == UPGRADE_STAGE_REBOOT_PENDING))
  {
    return ReleaseOperation(UPGRADE_STAGE_BUSY);
  }
  CheckIdleTimeout();
  session.resume_error = UpgradeQuiesce_Resume(UPGRADE_STAGE_IDLE_TIMEOUT_MS);
  if (session.resume_error != UPGRADE_STAGE_OK)
  {
    return ReleaseOperation(session.resume_error);
  }
  result = CloseStorage();
  if (result != UPGRADE_STAGE_OK)
  {
    SetError(result);
    return ReleaseOperation(result);
  }
  // 释放占用，保留最后进度和首次失败原因供查询；不擦半包或清持久日志。
  session.phase = UPGRADE_STAGE_IDLE;
  return ReleaseOperation(UPGRADE_STAGE_OK);
}

// 暂停业务并提交 READY，提交不明时只读核对结果。
UpgradeStageResult UpgradeStage_Commit(uint32_t session_id)
{
  if (!UpgradeStage_ExecutorAllowed())
  {
    return UPGRADE_STAGE_MODE;
  }
  UpgradeStageResult result = TakeOperation();
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if ((session_id == 0U) || (session.session_id != session_id))
  {
    return ReleaseOperation(UPGRADE_STAGE_MODE);
  }
  if (session.phase == UPGRADE_STAGE_REBOOT_PENDING)
  {
    return ReleaseOperation(UPGRADE_STAGE_OK);
  }
  bool resolve_only = session.phase == UPGRADE_STAGE_QUIESCING;
  if (!resolve_only && (session.phase != UPGRADE_STAGE_VERIFIED))
  {
    return ReleaseOperation(UPGRADE_STAGE_MODE);
  }
  if (!resolve_only)
  {
    if (UpgradeStage_CancellationRequested(session_id))
    {
      return ReleaseOperation(FailSession(UPGRADE_STAGE_MODE));
    }
    if (!UpgradeQuiesce_IsReady())
    {
      return ReleaseOperation(UPGRADE_STAGE_NOT_READY);
    }
    taskENTER_CRITICAL();
    bool cancelled = cancel_requested;
    if (!cancelled)
    {
      session.phase = UPGRADE_STAGE_QUIESCING;
      published_status = session; // 与取消请求的阶段检查原子衔接，避免接受取消后又提交。
    }
    taskEXIT_CRITICAL();
    if (cancelled)
    {
      return ReleaseOperation(FailSession(UPGRADE_STAGE_MODE));
    }
    commit_started_ms = HAL_GetTick();
    PublishStatus();
    result = UpgradeQuiesce_Run(commit_started_ms, UPGRADE_STAGE_COMMIT_TIMEOUT_MS);
  }
  uint32_t remaining = RemainingMs(commit_started_ms, UPGRADE_STAGE_COMMIT_TIMEOUT_MS);
  if ((result == UPGRADE_STAGE_OK) && !resolve_only && (remaining == 0U))
  {
    result = UPGRADE_STAGE_TIMEOUT;
  }
  if (result == UPGRADE_STAGE_OK)
  {
    UpgradeStatePackage candidate = {session.slot, session.total_size, {0}};
    memcpy(candidate.digest, verified_image.digest, sizeof(candidate.digest));
    // 原提交到期后的调用只准只读解析，另给3s诊断窗口，不续期或重发写操作。
    session.commit_state = UPGRADE_COMMIT_UNKNOWN;
    result = backend.commit(backend_context, session_id, &candidate, resolve_only,
                              &session.commit_state, resolve_only ? 3000U : remaining);
    if (session.commit_state == UPGRADE_COMMIT_WRITTEN)
    {
      session.phase = UPGRADE_STAGE_REBOOT_PENDING;
      reboot_pending_ms = HAL_GetTick();
      return ReleaseOperation(UPGRADE_STAGE_OK);
    }
    if (session.commit_state == UPGRADE_COMMIT_UNKNOWN)
    {
      if (session.error == UPGRADE_STAGE_OK)
      {
        session.error = result == UPGRADE_STAGE_OK ? UPGRADE_STAGE_INTERNAL : result;
      }
      // 不明提交不能恢复业务、清占用或开始另一包，留QUIESCING供显式解析。
      return ReleaseOperation(session.error);
    }
    if (result == UPGRADE_STAGE_OK)
    {
      result = UPGRADE_STAGE_INTERNAL;
    }
  }
  session.resume_error = UpgradeQuiesce_Resume(UPGRADE_STAGE_IDLE_TIMEOUT_MS);
  return ReleaseOperation(FailSession(result));
}

// 仅在当前会话 READY 已持久化后执行系统复位。
UpgradeStageResult UpgradeStage_Reboot(uint32_t session_id)
{
  UpgradeStageResult result = TakeOperation();
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  bool allowed = (session_id == session.session_id) && (session_id != 0U)
              && (session.phase == UPGRADE_STAGE_REBOOT_PENDING)
              && (session.commit_state == UPGRADE_COMMIT_WRITTEN);
  (void)ReleaseOperation(allowed ? UPGRADE_STAGE_OK : UPGRADE_STAGE_MODE);
  if (allowed)
  {
    UpgradeMailbox_Clear((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS);
    __DSB();
    NVIC_SystemReset();
  }
  return UPGRADE_STAGE_MODE;
}

// 检查接收超时，并执行已提交升级的延迟复位。
void UpgradeStage_Poll(void)
{
  if (TakeOperation() == UPGRADE_STAGE_OK)
  {
    CheckIdleTimeout();
    bool reboot = (session.phase == UPGRADE_STAGE_REBOOT_PENDING)
               && (session.commit_state == UPGRADE_COMMIT_WRITTEN)
               && ((uint32_t)(HAL_GetTick() - reboot_pending_ms) >= 1000U);
    uint32_t session_id = session.session_id;
    (void)ReleaseOperation(UPGRADE_STAGE_OK);
    if (reboot)
    {
      (void)UpgradeStage_Reboot(session_id);
    }
  }
}

// 检查 RAM 中的到期状态，返回一致的会话快照。
UpgradeStageResult UpgradeStage_GetStatus(UpgradeStageStatus *status)
{
  if (status == NULL)
  {
    return UPGRADE_STAGE_INVALID;
  }
  memset(status, 0, sizeof(*status));
  if (!TaskContext())
  {
    return UPGRADE_STAGE_MODE;
  }
  // 查询只做RAM状态到期检查，不执行Poll中的已提交重启动作或任何存储I/O。
  if (TakeOperation() == UPGRADE_STAGE_OK)
  {
    CheckIdleTimeout();
    (void)ReleaseOperation(UPGRADE_STAGE_OK);
  }
  taskENTER_CRITICAL();
  bool ready = initialized;
  *status = published_status;
  status->busy = operation_busy;
  taskEXIT_CRITICAL();
  return ready ? UPGRADE_STAGE_OK : UPGRADE_STAGE_NOT_READY;
}
