#include "upgrade_storage.h"
#include "upgrade_storage_protocol.h"
#include "upgrade_stage_auth.h"
#include "upgrade_trust.h"
#include "app_upgrade.h"
#include "app_monitor.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

/*
 * 将 Stage 同步调用转换为值拷贝队列请求；仅存储任务执行 FatFs 和外部 Flash 操作。
 * user_session 属于 Stage，storage_session 属于存储服务，不能混用或只凭槽号认领应答。
 */
typedef struct
{
  osMessageQueueId_t requests;
  osMessageQueueId_t responses;
  bool bound;
  bool begin_sent; // BEGIN 已入队，超时也可能已建立租约，清理时仍需向存储端核对。
  bool commit_sent; // READY 提交已入队，未确认结果前禁止再次发送提交。
  bool source_read;
  uint32_t next_request_id;
  uint32_t user_session;
  uint32_t storage_session;
  uint32_t slot;
  uint32_t total_size;
  uint32_t written;
  uint64_t tx_id;
  uint32_t started_ms;
  uint32_t timeout_ms;
  UpgradeStageResult io_error;
  UpgradeStateRecord baseline; // 准备时取得的可信普通记录，供后端执行前复核。
  UpgradeStateRecord ready; // 本次期望落盘的完整 READY 记录，供独立读回比较。
  UpgradeStorageRequest request;
  UpgradeStorageResponse response;
} StorageContext;

// 队列消息工作区位于普通 SRAM，由 UpgradeStage 的唯一修改操作独占。
static StorageContext storage;
static UpgradeStorageDiagnostic diagnostic;
static const UpgradeStageBackend backend;

// 计算当前存储操作总期限内剩余的毫秒数。
static uint32_t Remaining(void)
{
  uint32_t elapsed = (uint32_t)(HAL_GetTick() - storage.started_ms);
  return elapsed < storage.timeout_ms ? storage.timeout_ms - elapsed : 0U;
}

// 记录本次操作起点和总期限。
static void StartOperation(uint32_t timeout_ms)
{
  storage.started_ms = HAL_GetTick();
  storage.timeout_ms = timeout_ms;
  storage.io_error = UPGRADE_STAGE_OK;
}

// 将剩余毫秒换算为有界的队列等待 tick。
static uint32_t WaitTicks(uint32_t remaining_ms)
{
  uint64_t ticks = ((uint64_t)remaining_ms * osKernelGetTickFreq() + 999U) / 1000U;
  return ticks > 0xFFFFFFFEU ? 0xFFFFFFFEU : (uint32_t)ticks;
}

// 将存储服务应答码转换为升级服务结果码。
static UpgradeStageResult ResponseResult(int32_t result)
{
  switch (result)
  {
    case UPGRADE_STORAGE_OK: return UPGRADE_STAGE_OK;
    case UPGRADE_STORAGE_INVALID: return UPGRADE_STAGE_INVALID;
    case UPGRADE_STORAGE_BUSY: return UPGRADE_STAGE_BUSY;
    case UPGRADE_STORAGE_TIMEOUT: return UPGRADE_STAGE_TIMEOUT;
    case UPGRADE_STORAGE_CONFLICT: return UPGRADE_STAGE_MODE;
    case UPGRADE_STORAGE_NOT_READY:
    case UPGRADE_STORAGE_UNSUPPORTED: return UPGRADE_STAGE_NOT_READY;
    case UPGRADE_STORAGE_CORRUPT: return UPGRADE_STAGE_INTEGRITY;
    case UPGRADE_STORAGE_IO_ERROR: return UPGRADE_STAGE_STORAGE;
    default: return UPGRADE_STAGE_INTERNAL;
  }
}

// 清零请求缓冲并设置操作类型。
static void Request(UpgradeStorageRequestType type)
{
  memset(&storage.request, 0, sizeof(storage.request));
  storage.request.type = (uint8_t)type;
}

// 将当前保护快照身份写入请求，供存储端复核。
static void ExpectBaseline(void)
{
  storage.request.expected_normal_result = UPGRADE_STATE_OK;
  storage.request.expected_normal_sequence = storage.baseline.sequence;
  storage.request.expected_recovery_result = UPGRADE_STATE_EMPTY;
}

// 发布最近请求及应答的诊断快照。
static void PublishDiagnostic(bool waiting, UpgradeStageResult result)
{
  UpgradeStorageDiagnostic snapshot = {0};
  snapshot.req_id = storage.request.req_id;
  snapshot.type = storage.request.type;
  snapshot.offset = storage.request.offset;
  snapshot.slot = ((storage.request.type == UPGRADE_STORAGE_BEGIN)
                    || (storage.request.type == UPGRADE_STORAGE_READ_PACKAGE)
                    || (storage.request.type == UPGRADE_STORAGE_COMMIT_READY))
                ? (uint32_t)storage.request.slot + 1U : 0U;
  if (storage.request.type == UPGRADE_STORAGE_CHUNK)
  {
    snapshot.slot = storage.slot;
  }
  snapshot.tx_id = storage.request.tx_id;
  snapshot.waiting = waiting;
  snapshot.result = result;
  if (!waiting && (storage.response.req_id == storage.request.req_id)
      && (storage.response.type == storage.request.type) && (storage.request.req_id != 0U))
  {
    snapshot.peer_result = storage.response.result;
    snapshot.detail = storage.response.detail;
    snapshot.actual_len = storage.response.actual_len;
  }
  taskENTER_CRITICAL();
  diagnostic = snapshot;
  taskEXIT_CRITICAL();
}

// 等待匹配请求的应答，丢弃迟到消息并检查原期限。
static UpgradeStageResult AwaitResponse(bool *enqueued)
{
  memset(&storage.response, 0, sizeof(storage.response));
  if (enqueued != NULL)
  {
    *enqueued = false;
  }
  if ((storage.request.type != UPGRADE_STORAGE_ABORT)
      && UpgradeStage_CancellationRequested(storage.user_session))
  {
    return UPGRADE_STAGE_MODE;
  }
  if (storage.next_request_id == UINT32_MAX)
  {
    return UPGRADE_STAGE_MODE;
  }
  storage.request.req_id = ++storage.next_request_id;
  storage.request.deadline_ms = storage.started_ms + storage.timeout_ms;
  PublishDiagnostic(true, UPGRADE_STAGE_OK);
  uint32_t remaining = Remaining();
  if (remaining == 0U)
  {
    return UPGRADE_STAGE_TIMEOUT;
  }
  // 消息按值进入队列，调用者缓冲不借给存储任务；超时不代表服务端操作已撤销。
  osStatus_t status = osMessageQueuePut(storage.requests, &storage.request, 0U, WaitTicks(remaining));
  if (status != osOK)
  {
    return (status == osErrorTimeout || status == osErrorResource)
         ? UPGRADE_STAGE_TIMEOUT : UPGRADE_STAGE_STORAGE;
  }
  if (enqueued != NULL)
  {
    *enqueued = true;
  }
  for (;;)
  {
    if ((storage.request.type != UPGRADE_STORAGE_ABORT)
        && UpgradeStage_CancellationRequested(storage.user_session))
    {
      return UPGRADE_STAGE_MODE;
    }
    remaining = Remaining();
    if (remaining == 0U)
    {
      return UPGRADE_STAGE_TIMEOUT;
    }
    uint32_t slice_ms = remaining < 250U ? remaining : 250U;
    status = osMessageQueueGet(storage.responses, &storage.response, NULL, WaitTicks(slice_ms));
    if ((status == osErrorTimeout) && (Remaining() != 0U))
    {
      continue;
    }
    if (status != osOK)
    {
      return (status == osErrorTimeout || status == osErrorResource)
           ? UPGRADE_STAGE_TIMEOUT : UPGRADE_STAGE_STORAGE;
    }
    if (Remaining() == 0U)
    {
      return UPGRADE_STAGE_TIMEOUT;
    }
    if ((storage.response.req_id != storage.request.req_id)
        || (storage.response.type != storage.request.type))
    {
      continue; // 丢弃迟到应答，但仍使用原起点，不能每丢一条重新计时。
    }
    UpgradeStageResult result = ResponseResult(storage.response.result);
    bool new_session = (storage.request.type == UPGRADE_STORAGE_BEGIN) && (result == UPGRADE_STAGE_OK);
    bool new_tx = (storage.request.type == UPGRADE_STORAGE_RESERVE_TX) && (result == UPGRADE_STAGE_OK);
    if ((!new_session && (storage.response.session != storage.request.session))
        || (!new_tx && (storage.response.tx_id != storage.request.tx_id))
        || (new_session && (storage.response.session == 0U))
        || (new_tx && (storage.response.tx_id == 0U)))
    {
      return UPGRADE_STAGE_INTEGRITY;
    }
    return result;
  }
}

// 仅对升级任务登记本次队列等待；返回处理结果后报告实际进展。
static UpgradeStageResult WaitResponse(bool *enqueued)
{
  if (enqueued != NULL)
  {
    *enqueued = false;
  }
  uint32_t remaining = Remaining();
  if (remaining == 0U)
  {
    return UPGRADE_STAGE_TIMEOUT;
  }
  if (!Monitor_UpgradeWait(remaining))
  {
    return UPGRADE_STAGE_INTERNAL;
  }
  UpgradeStageResult result = AwaitResponse(enqueued);
  return Monitor_UpgradeProgress() ? result : UPGRADE_STAGE_INTERNAL;
}

// 在共享期限内提交请求并等待对应应答。
static UpgradeStageResult Exchange(bool *enqueued)
{
  UpgradeStageResult result = WaitResponse(enqueued);
  PublishDiagnostic(false, result);
  return result;
}

// 调用方已设请求类型/槽/会话并校验范围；size为1～256 B，data可写size B。
static UpgradeStageResult ReadResponse(uint32_t offset, uint8_t *data, uint32_t size)
{
  storage.request.offset = offset;
  storage.request.len = (uint16_t)size;
  UpgradeStageResult result = WaitResponse(NULL);
  if ((result == UPGRADE_STAGE_OK) && (storage.response.actual_len != size))
  {
    result = UPGRADE_STAGE_INTEGRITY;
  }
  if (result == UPGRADE_STAGE_OK)
  {
    memcpy(data, storage.response.data, size);
  }
  PublishDiagnostic(false, result); // 长度不符也记录为失败，不发布虚假的读取成功。
  return result;
}

// 在临界区复制存储适配诊断信息。
UpgradeStageResult UpgradeStorage_GetDiagnostic(UpgradeStorageDiagnostic *output)
{
  if (output == NULL)
  {
    return UPGRADE_STAGE_INVALID;
  }
  memset(output, 0, sizeof(*output));
  if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning))
  {
    return UPGRADE_STAGE_MODE;
  }
  taskENTER_CRITICAL();
  *output = diagnostic;
  taskEXIT_CRITICAL();
  return storage.bound ? UPGRADE_STAGE_OK : UPGRADE_STAGE_NOT_READY;
}

// 通过存储队列读取升级元数据，并保留首次读取错误。
static bool ReadMetadata(void *context, uint32_t offset, uint8_t *data, uint32_t size)
{
  (void)context;
  if ((data == NULL) || (size == 0U) || (size > UPGRADE_STORAGE_CHUNK_SIZE)
      || (offset >= UPGRADE_STATE_METADATA_SIZE) || (size > UPGRADE_STATE_METADATA_SIZE - offset))
  {
    storage.io_error = UPGRADE_STAGE_INVALID;
    return false;
  }
  Request(UPGRADE_STORAGE_READ_METADATA);
  UpgradeStageResult result = ReadResponse(offset, data, size);
  if (result != UPGRADE_STAGE_OK)
  {
    if (storage.io_error == UPGRADE_STAGE_OK)
    {
      storage.io_error = result;
    }
    return false;
  }
  return true;
}

// 读取共享日志保护视图，优先返回底层读取错误。
static UpgradeStageResult LoadProtection(UpgradeStateProtection *protection)
{
  UpgradeStateResult result = UpgradeState_ReadProtection(ReadMetadata, NULL, protection);
  if (storage.io_error != UPGRADE_STAGE_OK)
  {
    return storage.io_error;
  }
  if (result == UPGRADE_STATE_OK)
  {
    return UPGRADE_STAGE_OK;
  }
  if (result == UPGRADE_STATE_IO_ERROR)
  {
    return UPGRADE_STAGE_STORAGE;
  }
  return result == UPGRADE_STATE_EMPTY || result == UPGRADE_STATE_UNSUPPORTED
       ? UPGRADE_STAGE_NOT_READY : UPGRADE_STAGE_INTEGRITY;
}

// 通过存储队列读取所选包槽的指定字节区间。
static UpgradeStageResult Read(void *context, uint32_t session_id, uint32_t offset,
                                 uint8_t *data, uint32_t size, uint32_t timeout_ms)
{
  (void)context;
  bool confirmed = storage.source_read && (session_id == 0U);
  if ((!confirmed && ((session_id != storage.user_session) || (storage.storage_session == 0U)))
      || (data == NULL) || (size == 0U))
  {
    return UPGRADE_STAGE_MODE;
  }
  uint32_t limit = confirmed ? storage.baseline.confirmed.size : storage.written;
  if ((offset > limit) || (size > limit - offset))
  {
    return UPGRADE_STAGE_OFFSET;
  }
  // prepare内的旧源认证共用prepare原预算；普通候选读则使用调用方剩余期限。
  if (!confirmed)
  {
    StartOperation(timeout_ms);
  }
  uint32_t done = 0U;
  while (done < size)
  {
    uint32_t count = size - done;
    if (count > UPGRADE_STORAGE_CHUNK_SIZE)
    {
      count = UPGRADE_STORAGE_CHUNK_SIZE;
    }
    Request(UPGRADE_STORAGE_READ_PACKAGE);
    storage.request.slot = (uint8_t)((confirmed ? storage.baseline.confirmed.slot : storage.slot) - 1U);
    storage.request.session = confirmed ? 0U : storage.storage_session;
    storage.request.tx_id = confirmed ? 0U : storage.tx_id;
    UpgradeStageResult result = ReadResponse(offset + done, data + done, count);
    if (result != UPGRADE_STAGE_OK)
    {
      return result;
    }
    done += count;
  }
  return UPGRADE_STAGE_OK;
}

// 按包头编号查询可信签名和解密密钥。
static UpgradeStageResult Keys(void *context, uint32_t signing_key_id, uint32_t decrypt_key_id,
                                 const uint8_t **public_key, const uint8_t **aes_key)
{
  (void)context;
  if (!UpgradeTrust_IsReady())
  {
    return UPGRADE_STAGE_NOT_READY;
  }
  return UpgradeTrust_Get(signing_key_id, decrypt_key_id, public_key, aes_key)
       ? UPGRADE_STAGE_OK : UPGRADE_STAGE_AUTH;
}

/*
 * UpgradeStage_Begin 经 backend.prepare 进入这里，先证明旧包可用于回退，再准备新候选槽。
 * 候选槽按持久保护视图选择；传输方只提供包长，不能指定擦写 A 或 B。
 * 输出 Stage 使用的槽号和持久事务号，存储会话号另存于适配器，不能与上层 session_id 混用。
 */
static UpgradeStageResult Prepare(void *context, uint32_t session_id, uint32_t total_size,
                                    UpgradeStagePrepared *prepared, uint32_t timeout_ms)
{
  (void)context;
  memset(prepared, 0, sizeof(*prepared));
  if (storage.begin_sent || storage.commit_sent)
  {
    return UPGRADE_STAGE_BUSY;
  }
  storage.user_session = session_id;
  storage.storage_session = 0U;
  storage.tx_id = 0U;
  storage.total_size = total_size;
  storage.written = 0U;
  StartOperation(timeout_ms);
  // 1. 通过存储队列读取保护状态，核对 Boot 交接身份与仍然有效的已确认基线。
  UpgradeStateProtection protection;
  UpgradeStageResult result = LoadProtection(&protection);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (protection.writable_slots == 0U)
  {
    return UPGRADE_STAGE_MODE;
  }
  UpgradeMessage identity;
  if (!App_UpgradeGetBootIdentity(&identity)
      || (identity.sequence != protection.normal.sequence) || (identity.tx_id != protection.normal.tx_id)
      || (identity.trial_attempts != protection.normal.trial_attempts)
      || (memcmp(identity.digest, protection.normal.confirmed.digest, sizeof(identity.digest)) != 0))
  {
    return UPGRADE_STAGE_MODE;
  }
  // Boot 交接身份必须仍匹配持久已确认记录，防止基于过期视图挑选可擦槽。
  storage.baseline = protection.normal;
  storage.slot = (protection.writable_slots & UPGRADE_STATE_SLOT_A_MASK) != 0U ? 1U : 2U;

  // 2. 先认证回退源，再请求擦除候选槽；日志 CRC 不能替代包认证。
  storage.source_read = true;
  UpgradePackageImage image;
  result = UpgradeStage_Authenticate(&backend, NULL, 0U, storage.baseline.confirmed.size,
                                      storage.started_ms, &image);
  storage.source_read = false;
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (memcmp(image.digest, storage.baseline.confirmed.digest, sizeof(image.digest)) != 0)
  {
    return UPGRADE_STAGE_INTEGRITY;
  }
  // 3. 持久预留新的事务号，断电或准备失败也不能把已经消耗的事务号拿来复用。
  Request(UPGRADE_STORAGE_RESERVE_TX);
  result = Exchange(NULL);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  storage.tx_id = storage.response.tx_id;
  if (storage.tx_id <= storage.baseline.tx_id)
  {
    return UPGRADE_STAGE_INTEGRITY;
  }
  // 4. 把选槽和预期基线发给存储端；存储端执行时再次复核，通过后才实际擦除候选槽。
  Request(UPGRADE_STORAGE_BEGIN);
  storage.request.pkg_len = total_size;
  storage.request.tx_id = storage.tx_id;
  // Stage 使用 1/A、2/B，队列协议使用 0/A、1/B，跨边界时显式转换。
  storage.request.slot = (uint8_t)(storage.slot - 1U);
  ExpectBaseline();
  result = Exchange(&storage.begin_sent);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  storage.storage_session = storage.response.session;
  if (storage.response.written != 0U)
  {
    return UPGRADE_STAGE_INTEGRITY;
  }
  prepared->slot = storage.slot;
  prepared->tx_id = storage.tx_id;
  return UPGRADE_STAGE_OK;
}

/*
 * Stage 的 backend.write 适配：将一次最多 1016 B 输入拆成每条最多 256 B 的存储 RPC。
 * 这是队列消息的数据容量，Flash 页边界仍由存储侧驱动处理；1016 B 会拆成 256+256+256+248 B。
 * 每条都按值复制并等待对应应答；任一失败立即返回，不把入队成功当作写入成功。
 */
static UpgradeStageResult Write(void *context, uint32_t session_id, uint32_t offset,
                                  const uint8_t *data, uint32_t size, uint32_t timeout_ms)
{
  (void)context;
  if ((session_id != storage.user_session) || (storage.storage_session == 0U)
      || (data == NULL) || (size == 0U) || (offset != storage.written)
      || (offset > storage.total_size) || (size > storage.total_size - offset))
  {
    return UPGRADE_STAGE_OFFSET;
  }
  StartOperation(timeout_ms);
  uint32_t done = 0U;
  while (done < size)
  {
    uint32_t count = size - done;
    if (count > UPGRADE_STORAGE_CHUNK_SIZE)
    {
      count = UPGRADE_STORAGE_CHUNK_SIZE;
    }
    Request(UPGRADE_STORAGE_CHUNK);
    storage.request.session = storage.storage_session;
    storage.request.tx_id = storage.tx_id;
    storage.request.offset = offset + done;
    storage.request.len = (uint16_t)count;
    // 先复制到请求自带数组；Exchange 再把整条消息入队，不向存储任务借出调用方指针。
    memcpy(storage.request.data, data + done, count);
    UpgradeStageResult result = Exchange(NULL);
    if (result != UPGRADE_STAGE_OK)
    {
      return result;
    }
    // 存储端写入并读回后返回累计 written；必须恰好到达本次请求末尾才推进下一小块。
    if (storage.response.written != offset + done + count)
    {
      return UPGRADE_STAGE_INTEGRITY;
    }
    done += count;
    storage.written = offset + done;
  }
  return UPGRADE_STAGE_OK;
}

// 请求结束存储会话，明确释放后清除本地占用。
static UpgradeStageResult Close(void *context, uint32_t session_id, uint32_t timeout_ms)
{
  (void)context;
  if (session_id != storage.user_session)
  {
    return UPGRADE_STAGE_MODE;
  }
  if (!storage.begin_sent)
  {
    return UPGRADE_STAGE_OK;
  }
  StartOperation(timeout_ms);
  Request(UPGRADE_STORAGE_ABORT);
  storage.request.session = storage.storage_session;
  storage.request.tx_id = storage.tx_id;
  UpgradeStageResult result = Exchange(NULL);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (storage.response.session_ended != 1U)
  {
    return UPGRADE_STAGE_BUSY;
  }
  storage.begin_sent = false;
  storage.commit_sent = false;
  storage.storage_session = 0U;
  return UPGRADE_STAGE_OK;
}

// 比较已读回的 READY 记录是否属于本次候选事务。
static bool SameReady(const UpgradeStateRecord *a, const UpgradeStateRecord *b)
{
  return (a->sequence == b->sequence) && (a->phase == b->phase) && (a->tx_id == b->tx_id)
      && (a->trial_attempts == b->trial_attempts) && (a->operation == b->operation)
      && UpgradeState_PackageEqual(&a->confirmed, &b->confirmed)
      && UpgradeState_PackageEqual(&a->candidate, &b->candidate);
}

/*
 * Stage 的 backend.commit 适配：提交候选 READY 并读回完整持久身份，返回状态与错误码分别判断。
 * 尚未入队可明确 NOT_WRITTEN；已入队而结果未核实则保持 UNKNOWN，不能凭超时断言未写。
 * 已发送提交后的调用只读解析：目标 READY 完整匹配为 WRITTEN，原完整基线仍在才为 NOT_WRITTEN。
 */
static UpgradeStageResult Commit(void *context, uint32_t session_id,
                                   const UpgradeStatePackage *candidate, bool resolve_only,
                                   UpgradeStageCommitState *state, uint32_t timeout_ms)
{
  (void)context;
  *state = storage.commit_sent ? UPGRADE_COMMIT_UNKNOWN : UPGRADE_COMMIT_NOT_WRITTEN;
  if ((session_id != storage.user_session) || (storage.storage_session == 0U)
      || (candidate->slot != storage.slot) || (candidate->size != storage.total_size))
  {
    return UPGRADE_STAGE_MODE;
  }
  StartOperation(timeout_ms);
  UpgradeStateProtection protection;
  UpgradeStageResult result = LoadProtection(&protection);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (storage.commit_sent)
  {
    // 提交只发一次；本次 LoadProtection 同样经 Storage 队列，元数据读取排在原提交操作之后。
    if ((protection.recovery_result == UPGRADE_STATE_EMPTY)
        && SameReady(&protection.normal, &storage.ready))
    {
      *state = UPGRADE_COMMIT_WRITTEN;
      return UPGRADE_STAGE_OK;
    }
    if (!SameReady(&protection.normal, &storage.baseline)
        || (protection.recovery_result != UPGRADE_STATE_EMPTY))
    {
      return UPGRADE_STAGE_INTEGRITY;
    }
    // 新 READY 不匹配时仍须证明原完整基线未变；读错误、身份冲突或恢复屏障都不能判为未提交。
    *state = UPGRADE_COMMIT_NOT_WRITTEN;
    return UPGRADE_STAGE_STORAGE;
  }
  if (resolve_only)
  {
    return UPGRADE_STAGE_MODE;
  }
  if (!SameReady(&protection.normal, &storage.baseline)
      || (protection.recovery_result != UPGRADE_STATE_EMPTY)
      || (storage.baseline.sequence == UINT32_MAX))
  {
    return UPGRADE_STAGE_MODE;
  }
  // 保留已确认旧包，只替换候选描述；序号递增并关联已预留的持久事务号。
  storage.ready = storage.baseline;
  storage.ready.sequence++;
  storage.ready.phase = UPGRADE_STATE_READY;
  storage.ready.operation = UPGRADE_OPERATION_INSTALL;
  storage.ready.trial_attempts = 0U;
  storage.ready.tx_id = storage.tx_id;
  storage.ready.candidate = *candidate;
  Request(UPGRADE_STORAGE_COMMIT_READY);
  storage.request.session = storage.storage_session;
  storage.request.tx_id = storage.tx_id;
  storage.request.slot = (uint8_t)(storage.slot - 1U);
  storage.request.len = UPGRADE_STATE_RECORD_SIZE;
  ExpectBaseline();
  if (!UpgradeState_TransitionValid(&storage.baseline, &storage.ready)
      || !UpgradeState_Encode(&storage.ready, storage.ready.sequence, storage.request.data))
  {
    return UPGRADE_STAGE_INTEGRITY;
  }
  // commit_sent 只表示请求已入队；即使等待超时或收到失败应答，也先按 UNKNOWN 交给后续只读核对。
  result = Exchange(&storage.commit_sent);
  *state = storage.commit_sent ? UPGRADE_COMMIT_UNKNOWN : UPGRADE_COMMIT_NOT_WRITTEN;
  if (result == UPGRADE_STAGE_OK)
  {
    // 成功应答本身按共同契约已含提交读回，仍经独立读取核对实际记录。
    result = LoadProtection(&protection);
    if ((result == UPGRADE_STAGE_OK) && (protection.recovery_result == UPGRADE_STATE_EMPTY)
        && SameReady(&protection.normal, &storage.ready))
    {
      *state = UPGRADE_COMMIT_WRITTEN;
    }
    else if (result == UPGRADE_STAGE_OK)
    {
      result = UPGRADE_STAGE_INTEGRITY;
    }
  }
  return result;
}

// 调用方持有Stage串行占用，只通过存储任务读取日志，不建立收包会话。
static UpgradeStageResult ReadBootState(void *context, UpgradeStateProtection *state,
                                         uint32_t timeout_ms)
{
  (void)context;
  if (storage.begin_sent || storage.commit_sent)
  {
    return UPGRADE_STAGE_BUSY;
  }
  StartOperation(timeout_ms);
  return LoadProtection(state);
}

// Stage 调用此函数表，Prepare/Write/Read/Commit 再把升级操作转成存储请求和应答。
static const UpgradeStageBackend backend = {Prepare, Write, Read, Keys, Close, Commit, ReadBootState};

// 检查基线、协议尺寸、真实队列及密钥，接入升级后端。
UpgradeStageResult UpgradeStorage_Bind(const UpgradeStorageBinding *binding)
{
  // 存储侧提供已经建立的队列；此处只校验并绑定，不另建存储任务或直接访问 Flash。
  if ((binding == NULL) || (binding->requests == NULL) || (binding->responses == NULL))
  {
    return UPGRADE_STAGE_INVALID;
  }
  if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning))
  {
    return UPGRADE_STAGE_MODE;
  }
  if (storage.bound)
  {
    return UPGRADE_STAGE_BUSY;
  }
  if (!UpgradeTrust_IsReady()
      || (binding->protocol_version != UPGRADE_STORAGE_PROTOCOL_VERSION)
      || (osMessageQueueGetMsgSize(binding->requests) != sizeof(UpgradeStorageRequest))
      || (osMessageQueueGetMsgSize(binding->responses) != sizeof(UpgradeStorageResponse)))
  {
    return UPGRADE_STAGE_NOT_READY;
  }
  // 请求队列交给存储任务消费，应答队列供升级执行任务等待；消息按值拷贝。
  storage.requests = binding->requests;
  storage.responses = binding->responses;
  // 把上面的函数表交给 Stage，接通 Entry → Stage → 存储队列 → 存储任务这条链。
  UpgradeStageResult result = UpgradeStage_Bind(&backend, NULL);
  storage.bound = result == UPGRADE_STAGE_OK;
  return result;
}
