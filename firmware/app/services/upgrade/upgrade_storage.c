#include "upgrade_storage.h"
#include "upgrade_storage_protocol.h"
#include "upgrade_stage_auth.h"
#include "upgrade_trust.h"
#include "app_upgrade.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

typedef struct
{
  osMessageQueueId_t requests;
  osMessageQueueId_t responses;
  bool bound;
  bool begin_sent;
  bool commit_sent;
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
  UpgradeStateRecord baseline;
  UpgradeStateRecord ready;
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
static UpgradeStageResult WaitResponse(bool *enqueued)
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

// 认证已确认恢复源并复核启动身份，预留事务后准备候选槽。
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
  storage.baseline = protection.normal;
  storage.slot = (protection.writable_slots & UPGRADE_STATE_SLOT_A_MASK) != 0U ? 1U : 2U;

  // 先认证回退源，再请求擦除候选槽；日志 CRC 不能替代包认证。
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
  Request(UPGRADE_STORAGE_BEGIN);
  storage.request.pkg_len = total_size;
  storage.request.tx_id = storage.tx_id;
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

// 将输入分成存储页大小的请求，逐块确认写入偏移。
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
    memcpy(storage.request.data, data + done, count);
    UpgradeStageResult result = Exchange(NULL);
    if (result != UPGRADE_STAGE_OK)
    {
      return result;
    }
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

// 提交候选 READY 并读回核对，提交不明时仅解析现有记录。
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
    // 元数据读请求排在原提交之后；看到原完整基线才可判定未写，不盲目重发。
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

static const UpgradeStageBackend backend = {Prepare, Write, Read, Keys, Close, Commit};

// 检查基线、协议尺寸、真实队列及密钥，接入升级后端。
UpgradeStageResult UpgradeStorage_Bind(const UpgradeStorageBinding *binding)
{
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
  storage.requests = binding->requests;
  storage.responses = binding->responses;
  UpgradeStageResult result = UpgradeStage_Bind(&backend, NULL);
  storage.bound = result == UPGRADE_STAGE_OK;
  return result;
}
