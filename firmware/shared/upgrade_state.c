/*
 * Boot 和 App 共用的升级记录格式及保护规则；本文件通过回调读取，不直接驱动 Flash。
 * 普通记录、恢复记录各有双副本，正文 CRC 和独立提交标记共同决定记录是否完整。
 */
#include "upgrade_state.h"

#include <string.h>

_Static_assert(UPGRADE_STATE_NORMAL_OFFSET + 2U * UPGRADE_STATE_SECTOR_SIZE
                 == UPGRADE_STATE_RECOVERY_OFFSET, "Normal journal overlaps recovery");
_Static_assert(UPGRADE_STATE_RECOVERY_OFFSET + 2U * UPGRADE_STATE_SECTOR_SIZE
                 == UPGRADE_STATE_BARRIER_OFFSET, "Recovery journal overlaps barrier");
_Static_assert(UPGRADE_STATE_BARRIER_OFFSET + UPGRADE_STATE_SECTOR_SIZE
                 == UPGRADE_STATE_TRANSACTION_OFFSET, "Metadata extent mismatch");
_Static_assert((UPGRADE_STATE_COMMIT_OFFSET >= UPGRADE_STATE_RECORD_SIZE)
                 && (UPGRADE_STATE_COMMIT_OFFSET + UPGRADE_STATE_COMMIT_SIZE
                     <= UPGRADE_STATE_SECTOR_SIZE), "Commit marker outside its page/sector");
_Static_assert(UPGRADE_STATE_TRANSACTION_OFFSET + 2U * UPGRADE_STATE_SECTOR_SIZE
                 == UPGRADE_STATE_METADATA_SIZE, "Transaction extent mismatch");

// 从记录字节缓冲读取小端 32 位整数。
static uint32_t Read32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U)
       | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

// 按小端顺序将 32 位整数写入记录缓冲。
static void Write32(uint8_t *p, uint32_t value)
{
  for (uint32_t i = 0U; i < 4U; i++)
  {
    p[i] = (uint8_t)(value >> (8U * i));
  }
}

// 从两个小端 32 位字段还原 64 位整数。
static uint64_t Read64(const uint8_t *p)
{
  return (uint64_t)Read32(p) | ((uint64_t)Read32(p + 4U) << 32U);
}

// 将 64 位整数拆成两个小端 32 位字段。
static void Write64(uint8_t *p, uint64_t value)
{
  Write32(p, (uint32_t)value);
  Write32(p + 4U, (uint32_t)(value >> 32U));
}

// CRC-32/ISO-HDLC只检测记录意外损坏，不替代包认证。
static uint32_t RecordCrc(const uint8_t *p, uint32_t size)
{
  uint32_t crc = UINT32_MAX;
  for (uint32_t i = 0U; i < size; i++)
  {
    crc ^= p[i];
    for (uint32_t bit = 0U; bit < 8U; bit++)
    {
      crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
  }
  return ~crc;
}

// 检查缓冲中的字节是否全部等于指定值。
static bool AllValue(const uint8_t *p, uint32_t size, uint8_t value)
{
  for (uint32_t i = 0U; i < size; i++)
  {
    if (p[i] != value)
    {
      return false;
    }
  }
  return true;
}

// 检查包槽、长度及空包字段是否符合记录规则。
static bool PackageValid(const UpgradeStatePackage *package)
{
  if (package->slot == 0U)
  {
    return (package->size == 0U) && AllValue(package->digest, 32U, 0U);
  }
  return (package->slot <= 2U) && (package->size >= 216U)
      && (package->size <= 448U * 1024U + 208U);
}

// 检查普通升级状态中的阶段、包角色及事务字段。
bool UpgradeState_RecordValid(const UpgradeStateRecord *state)
{
  if (state == NULL)
  {
    return false;
  }
  if ((state->tx_id == 0U) || (state->trial_attempts > UPGRADE_TRIAL_LIMIT))
  {
    return false;
  }
  bool metadata_valid = false;
  switch (state->phase)
  {
    case UPGRADE_STATE_CONFIRMED:
      metadata_valid = (((state->operation == UPGRADE_OPERATION_ENROLL)
                          || (state->operation == UPGRADE_OPERATION_CANCEL)) && (state->trial_attempts == 0U))
                    || (state->operation == UPGRADE_OPERATION_RESTORE)
                    || (((state->operation == UPGRADE_OPERATION_INSTALL)
                          || (state->operation == UPGRADE_OPERATION_RECOVERY)) && (state->trial_attempts != 0U));
      break;
    case UPGRADE_STATE_READY:
    case UPGRADE_STATE_INSTALLING:
      metadata_valid = (state->operation == UPGRADE_OPERATION_INSTALL) && (state->trial_attempts == 0U);
      break;
    case UPGRADE_STATE_TRIAL:
      metadata_valid = (state->operation == UPGRADE_OPERATION_INSTALL) && (state->trial_attempts != 0U);
      break;
    case UPGRADE_STATE_RESTORING:
      metadata_valid = state->operation == UPGRADE_OPERATION_RESTORE;
      break;
    default:
      break;
  }
  if (!metadata_valid || ((state->phase == UPGRADE_STATE_READY) && (state->confirmed.slot == 0U)))
  {
    return false;
  }
  if (!PackageValid(&state->confirmed) || !PackageValid(&state->candidate)
      || ((state->confirmed.slot != 0U)
          && (state->confirmed.slot == state->candidate.slot)))
  {
    return false;
  }
  switch (state->phase)
  {
    case UPGRADE_STATE_CONFIRMED:
      return (state->confirmed.slot != 0U) && (state->candidate.slot == 0U);
    case UPGRADE_STATE_READY:
      return state->candidate.slot != 0U;
    case UPGRADE_STATE_INSTALLING:
    case UPGRADE_STATE_TRIAL:
      return (state->confirmed.slot != 0U) && (state->candidate.slot != 0U);
    case UPGRADE_STATE_RESTORING:
      return state->confirmed.slot != 0U;
    default:
      return false;
  }
}

// 将包描述编码为固定位置的槽号、长度和摘要。
static void EncodePackage(uint8_t *data, const UpgradeStatePackage *package)
{
  Write32(data, package->slot);
  Write32(data + 4U, package->size);
  memcpy(data + 8U, package->digest, 32U);
}

// 从记录缓冲解码包槽、长度和摘要。
static void DecodePackage(const uint8_t *data, UpgradeStatePackage *package)
{
  package->slot = Read32(data);
  package->size = Read32(data + 4U);
  memcpy(package->digest, data + 8U, 32U);
}

// 比较两份包描述的槽号、长度和摘要。
bool UpgradeState_PackageEqual(const UpgradeStatePackage *a, const UpgradeStatePackage *b)
{
  if ((a == NULL) || (b == NULL))
  {
    return false;
  }
  return (a->slot == b->slot) && (a->size == b->size)
      && (memcmp(a->digest, b->digest, 32U) == 0);
}

// 检查普通升级状态转换及包角色变化是否合法。
bool UpgradeState_TransitionValid(const UpgradeStateRecord *old, const UpgradeStateRecord *next)
{
  if (!UpgradeState_RecordValid(old) || !UpgradeState_RecordValid(next))
  {
    return false;
  }
  bool same_confirmed = UpgradeState_PackageEqual(&old->confirmed, &next->confirmed);
  bool same_candidate = UpgradeState_PackageEqual(&old->candidate, &next->candidate);
  if (old->phase == UPGRADE_STATE_CONFIRMED)
  {
    if ((next->tx_id <= old->tx_id) || (next->trial_attempts != 0U))
    {
      return false;
    }
  }
  else if (next->tx_id != old->tx_id)
  {
    return false;
  }
  if ((old->phase == UPGRADE_STATE_TRIAL) && (next->phase == UPGRADE_STATE_TRIAL))
  {
    return same_confirmed && same_candidate && (old->trial_attempts == 1U)
        && (next->trial_attempts == 2U);
  }
  uint32_t attempts = (old->phase == UPGRADE_STATE_INSTALLING) && (next->phase == UPGRADE_STATE_TRIAL)
                    ? 1U : old->trial_attempts;
  if ((old->phase != UPGRADE_STATE_CONFIRMED) && (next->trial_attempts != attempts))
  {
    return false;
  }
  if ((next->phase == UPGRADE_STATE_CONFIRMED)
      && (((old->phase == UPGRADE_STATE_READY) && (next->operation != UPGRADE_OPERATION_CANCEL))
          || ((old->phase == UPGRADE_STATE_TRIAL) && (next->operation != UPGRADE_OPERATION_INSTALL))
          || ((old->phase == UPGRADE_STATE_RESTORING) && (next->operation != UPGRADE_OPERATION_RESTORE))))
  {
    return false;
  }
  if (old->phase == UPGRADE_STATE_CONFIRMED)
  {
    if (!same_confirmed)
    {
      return false;
    }
    return (next->phase == UPGRADE_STATE_READY)
        || ((next->phase == UPGRADE_STATE_RESTORING) && same_candidate);
  }
  if (old->phase == UPGRADE_STATE_READY)
  {
    if (!same_confirmed)
    {
      return false;
    }
    if (next->phase == UPGRADE_STATE_CONFIRMED)
    {
      return true;
    }
    return same_candidate && ((next->phase == UPGRADE_STATE_INSTALLING)
                              || (next->phase == UPGRADE_STATE_RESTORING));
  }
  if (old->phase == UPGRADE_STATE_INSTALLING)
  {
    return same_confirmed && same_candidate
        && ((next->phase == UPGRADE_STATE_TRIAL) || (next->phase == UPGRADE_STATE_RESTORING));
  }
  if (old->phase == UPGRADE_STATE_TRIAL)
  {
    if (next->phase == UPGRADE_STATE_CONFIRMED)
    {
      // 只有试运行确认才能把候选提升为新的已确认包。
      return UpgradeState_PackageEqual(&old->candidate, &next->confirmed);
    }
    return (next->phase == UPGRADE_STATE_RESTORING) && same_confirmed && same_candidate;
  }
  return (old->phase == UPGRADE_STATE_RESTORING)
      && (next->phase == UPGRADE_STATE_CONFIRMED) && same_confirmed;
}

// 检查恢复记录的阶段、包信息及事务字段。
bool UpgradeRecovery_RecordValid(const UpgradeRecoveryRecord *state)
{
  if ((state == NULL) || (state->tx_id == 0U) || (state->operation != UPGRADE_OPERATION_RECOVERY)
      || (state->trial_attempts > UPGRADE_TRIAL_LIMIT)
      || ((state->phase <= UPGRADE_RECOVERY_INSTALLING) != (state->trial_attempts == 0U)))
  {
    return false;
  }
  return (state != NULL) && (state->phase >= UPGRADE_RECOVERY_RECEIVING)
      && (state->phase <= UPGRADE_RECOVERY_BLOCKED)
      && (state->package.slot >= 1U) && (state->package.slot <= 2U)
      && (state->package.size >= 216U) && (state->package.size <= 448U * 1024U + 208U);
}

// 检查恢复事务是否允许转换到目标状态。
bool UpgradeRecovery_TransitionValid(const UpgradeRecoveryRecord *old, const UpgradeRecoveryRecord *next)
{
  if (!UpgradeRecovery_RecordValid(old) || !UpgradeRecovery_RecordValid(next))
  {
    return false;
  }
  if (next->phase == UPGRADE_RECOVERY_RECEIVING)
  {
    return next->tx_id > old->tx_id;
  }
  if (next->tx_id != old->tx_id)
  {
    return false;
  }
  if ((old->phase == UPGRADE_RECOVERY_TRIAL) && (next->phase == UPGRADE_RECOVERY_TRIAL))
  {
    return UpgradeState_PackageEqual(&old->package, &next->package)
        && (old->trial_attempts == 1U) && (next->trial_attempts == 2U);
  }
  uint32_t attempts = (old->phase == UPGRADE_RECOVERY_INSTALLING) && (next->phase == UPGRADE_RECOVERY_TRIAL)
                    ? 1U : old->trial_attempts;
  if (next->trial_attempts != attempts)
  {
    return false;
  }
  if ((old->package.slot != next->package.slot) || (old->package.size != next->package.size))
  {
    return false;
  }
  if (old->phase != UPGRADE_RECOVERY_RECEIVING)
  {
    if (memcmp(old->package.digest, next->package.digest, 32U) != 0)
    {
      return false;
    }
  }
  switch (old->phase)
  {
    case UPGRADE_RECOVERY_RECEIVING:
      return next->phase == UPGRADE_RECOVERY_READY;
    case UPGRADE_RECOVERY_READY:
      return next->phase == UPGRADE_RECOVERY_INSTALLING;
    case UPGRADE_RECOVERY_INSTALLING:
      return next->phase == UPGRADE_RECOVERY_TRIAL;
    case UPGRADE_RECOVERY_TRIAL:
      return (next->phase == UPGRADE_RECOVERY_CONFIRMED) || (next->phase == UPGRADE_RECOVERY_BLOCKED);
    case UPGRADE_RECOVERY_CONFIRMED:
      return next->phase == UPGRADE_RECOVERY_DONE;
    default:
      return false;
  }
}

/*
 * 将普通状态编码成固定 128 B 小端正文，不直接复制 RAM 结构，也不写提交标记。
 * 调用方提供新的日志序号；CRC 覆盖前 124 B 并写入末 4 B，正文写完仍须另页提交。
 */
bool UpgradeState_Encode(const UpgradeStateRecord *state, uint32_t sequence, uint8_t *data)
{
  if (data == NULL)
  {
    return false;
  }
  memset(data, 0, UPGRADE_STATE_RECORD_SIZE);
  if (!UpgradeState_RecordValid(state) || (sequence == 0U))
  {
    return false;
  }
  memcpy(data, UPGRADE_STATE_NORMAL_MAGIC, 8U);
  Write32(data + 8U, UPGRADE_PERSISTENCE_VERSION);
  Write32(data + 12U, sequence);
  Write32(data + 16U, (uint32_t)state->phase);
  // 记录存的是包的位置和镜像身份，不包含固件本体；已确认包与候选包各占 40 B。
  EncodePackage(data + 20U, &state->confirmed);
  EncodePackage(data + 60U, &state->candidate);
  Write64(data + 100U, state->tx_id);
  Write32(data + 108U, state->trial_attempts);
  Write32(data + 112U, state->operation);
  Write32(data + 124U, RecordCrc(data, 124U));
  return true;
}

// 校验普通记录的格式和 CRC，再解码语义字段。
bool UpgradeState_Decode(const uint8_t *data, UpgradeStateRecord *state)
{
  if (state == NULL)
  {
    return false;
  }
  memset(state, 0, sizeof(*state));
  if ((data == NULL) || (memcmp(data, UPGRADE_STATE_NORMAL_MAGIC, 8U) != 0)
      || (Read32(data + 8U) != UPGRADE_PERSISTENCE_VERSION)
      || (Read32(data + 124U) != RecordCrc(data, 124U))
      || !AllValue(data + 116U, 8U, 0U))
  {
    return false;
  }
  UpgradeStateRecord decoded = {0};
  decoded.sequence = Read32(data + 12U);
  decoded.phase = (UpgradeStatePhase)Read32(data + 16U);
  DecodePackage(data + 20U, &decoded.confirmed);
  DecodePackage(data + 60U, &decoded.candidate);
  decoded.tx_id = Read64(data + 100U);
  decoded.trial_attempts = Read32(data + 108U);
  decoded.operation = (UpgradeStateOperation)Read32(data + 112U);
  if ((decoded.sequence == 0U) || !UpgradeState_RecordValid(&decoded))
  {
    return false;
  }
  *state = decoded;
  return true;
}

// 按当前持久基线编码恢复记录并生成 CRC。
bool UpgradeRecovery_Encode(const UpgradeRecoveryRecord *state, uint32_t sequence, uint8_t *data)
{
  if (data == NULL)
  {
    return false;
  }
  memset(data, 0, UPGRADE_STATE_RECORD_SIZE);
  if (!UpgradeRecovery_RecordValid(state) || (sequence == 0U))
  {
    return false;
  }
  memcpy(data, UPGRADE_STATE_RECOVERY_MAGIC, 8U);
  Write32(data + 8U, UPGRADE_PERSISTENCE_VERSION);
  Write32(data + 12U, sequence);
  Write32(data + 16U, (uint32_t)state->phase);
  Write32(data + 20U, state->package.slot);
  Write32(data + 24U, state->package.size);
  memcpy(data + 28U, state->package.digest, 32U);
  Write64(data + 60U, state->tx_id);
  Write32(data + 68U, state->trial_attempts);
  Write32(data + 72U, state->operation);
  Write32(data + 124U, RecordCrc(data, 124U));
  return true;
}

// 校验恢复记录的格式和 CRC，再解码语义字段。
bool UpgradeRecovery_Decode(const uint8_t *data, UpgradeRecoveryRecord *state)
{
  if (state == NULL)
  {
    return false;
  }
  memset(state, 0, sizeof(*state));
  if ((data == NULL) || (memcmp(data, UPGRADE_STATE_RECOVERY_MAGIC, 8U) != 0)
      || (Read32(data + 8U) != UPGRADE_PERSISTENCE_VERSION)
      || (Read32(data + 124U) != RecordCrc(data, 124U))
      || !AllValue(data + 76U, 48U, 0U))
  {
    return false;
  }
  UpgradeRecoveryRecord decoded = {0};
  decoded.sequence = Read32(data + 12U);
  decoded.phase = (UpgradeRecoveryPhase)Read32(data + 16U);
  decoded.package.slot = Read32(data + 20U);
  decoded.package.size = Read32(data + 24U);
  memcpy(decoded.package.digest, data + 28U, 32U);
  decoded.tx_id = Read64(data + 60U);
  decoded.trial_attempts = Read32(data + 68U);
  decoded.operation = (UpgradeStateOperation)Read32(data + 72U);
  if ((decoded.sequence == 0U) || !UpgradeRecovery_RecordValid(&decoded))
  {
    return false;
  }
  *state = decoded;
  return true;
}

// 读取一份记录正文和独立提交标记。
static bool ReadCopy(UpgradeStateRead read, void *context, uint32_t offset,
                      uint8_t *data, uint8_t *marker)
{
  return read(context, offset, data, UPGRADE_STATE_RECORD_SIZE)
      && read(context, offset + UPGRADE_STATE_COMMIT_OFFSET, marker, UPGRADE_STATE_COMMIT_SIZE);
}

// 仅完整且CRC有效的另一持久版本归为不支持；损坏不能冒充可迁移旧格式。
static bool UnsupportedRecord(const uint8_t *data)
{
  return (Read32(data + 124U) == RecordCrc(data, 124U))
      && ((memcmp(data, "GWSTATE", 7U) == 0) || (memcmp(data, "GWRCLOG", 7U) == 0))
      && (Read32(data + 8U) != UPGRADE_PERSISTENCE_VERSION);
}

/*
 * 各副本完成提交和格式检查后，按非回绕 sequence 选最新有效记录，不按固件版本选。
 * 没有有效副本时，仅两份均空白才是 EMPTY；序号相同不能凭位置或内容猜测谁更新。
 */
static UpgradeStateResult SelectCopy(const bool *valid, const bool *blank,
                                      uint32_t first_sequence, uint32_t second_sequence,
                                      uint32_t *selected)
{
  if (!valid[0] && !valid[1])
  {
    return (blank[0] && blank[1]) ? UPGRADE_STATE_EMPTY : UPGRADE_STATE_CORRUPT;
  }
  if (valid[0] && valid[1] && (first_sequence == second_sequence))
  {
    return UPGRADE_STATE_CORRUPT;
  }
  *selected = (!valid[0] || (valid[1] && (second_sequence > first_sequence))) ? 1U : 0U;
  return UPGRADE_STATE_OK;
}

/*
 * 逐份读取普通日志正文和独立标记，先判提交，再检查格式/CRC，最后按序号选择。
 * 未提交副本可忽略；已提交副本损坏时不能静默回退，因为该记录之后可能已擦写片内 App。
 * 本函数只提供持久状态，包和实际片内镜像的认证仍由 Boot 安装状态机完成。
 */
UpgradeStateResult UpgradeState_Load(UpgradeStateRead read, void *context,
                                    UpgradeStateRecord *state, uint32_t *selected)
{
  if (selected != NULL)
  {
    *selected = 0U;
  }
  if (state == NULL)
  {
    return UPGRADE_STATE_INVALID;
  }
  memset(state, 0, sizeof(*state));
  if (read == NULL)
  {
    return UPGRADE_STATE_INVALID;
  }
  uint8_t data[UPGRADE_STATE_RECORD_SIZE];
  uint8_t marker[UPGRADE_STATE_COMMIT_SIZE];
  UpgradeStateRecord decoded[2] = {0};
  bool valid[2] = {false, false};
  bool blank[2];
  for (uint32_t i = 0U; i < 2U; i++)
  {
    uint32_t offset = UPGRADE_STATE_NORMAL_OFFSET + i * UPGRADE_STATE_SECTOR_SIZE;
    // 1. 读取失败直接返回 I/O 错误，不能把未读到的数据当成空白或仅采用另一副本。
    if (!ReadCopy(read, context, offset, data, marker))
    {
      return UPGRADE_STATE_IO_ERROR;
    }
    blank[i] = AllValue(data, sizeof(data), 0xFFU) && AllValue(marker, sizeof(marker), 0xFFU);
    if (memcmp(marker, UPGRADE_STATE_NORMAL_COMMIT, sizeof(marker)) != 0)
    {
      // 2. 正文即使已写完，没有完整 CMIT 仍不参加选择；有效旧副本因此可继续保留。
      continue;
    }
    // 3. 完整 CMIT 的记录必须通过解码；损坏或不支持的格式均报错，不撤销已发生的状态推进。
    if (!UpgradeState_Decode(data, &decoded[i]))
    {
      return UnsupportedRecord(data) ? UPGRADE_STATE_UNSUPPORTED : UPGRADE_STATE_CORRUPT;
    }
    valid[i] = true;
  }
  // 4. 仅在上述检查全部完成后选择副本；同序号、无有效记录但非全空白等情况仍拒绝。
  uint32_t index = 0U;
  UpgradeStateResult result = SelectCopy(valid, blank, decoded[0].sequence, decoded[1].sequence, &index);
  if (result == UPGRADE_STATE_OK)
  {
    *state = decoded[index];
    if (selected != NULL)
    {
      *selected = index;
    }
  }
  return result;
}

// 读取恢复日志双副本，不检查启动屏障。
UpgradeStateResult UpgradeRecovery_LoadRecords(UpgradeStateRead read, void *context,
                                               UpgradeRecoveryRecord *state, uint32_t *selected)
{
  if (selected != NULL)
  {
    *selected = 0U;
  }
  if (state == NULL)
  {
    return UPGRADE_STATE_INVALID;
  }
  memset(state, 0, sizeof(*state));
  if (read == NULL)
  {
    return UPGRADE_STATE_INVALID;
  }
  uint8_t data[UPGRADE_STATE_RECORD_SIZE];
  uint8_t marker[UPGRADE_STATE_COMMIT_SIZE];
  UpgradeRecoveryRecord decoded[2] = {0};
  bool valid[2] = {false, false};
  bool blank[2];
  for (uint32_t i = 0U; i < 2U; i++)
  {
    uint32_t offset = UPGRADE_STATE_RECOVERY_OFFSET + i * UPGRADE_STATE_SECTOR_SIZE;
    if (!ReadCopy(read, context, offset, data, marker))
    {
      return UPGRADE_STATE_IO_ERROR;
    }
    blank[i] = AllValue(data, sizeof(data), 0xFFU) && AllValue(marker, sizeof(marker), 0xFFU);
    if (memcmp(marker, UPGRADE_STATE_RECOVERY_COMMIT, sizeof(marker)) != 0)
    {
      continue;
    }
    if (!UpgradeRecovery_Decode(data, &decoded[i]))
    {
      return UnsupportedRecord(data) ? UPGRADE_STATE_UNSUPPORTED : UPGRADE_STATE_CORRUPT;
    }
    valid[i] = true;
  }
  uint32_t index = 0U;
  UpgradeStateResult result = SelectCopy(valid, blank, decoded[0].sequence, decoded[1].sequence, &index);
  if (result == UPGRADE_STATE_OK)
  {
    *state = decoded[index];
    if (selected != NULL)
    {
      *selected = index;
    }
  }
  return result;
}

// 检查恢复屏障是否为空，决定能否写普通升级日志。
bool UpgradeRecovery_NormalWritesAllowed(UpgradeStateRead read, void *context)
{
  uint8_t barrier = 0U;
  return (read != NULL) && read(context, UPGRADE_STATE_BARRIER_OFFSET, &barrier, 1U)
      && (barrier == 0xFFU);
}

// 先检查启动屏障，再加载恢复记录。
UpgradeStateResult UpgradeRecovery_Load(UpgradeStateRead read, void *context,
                                        UpgradeRecoveryRecord *state)
{
  if (state == NULL)
  {
    return UPGRADE_STATE_INVALID;
  }
  memset(state, 0, sizeof(*state));
  if (read == NULL)
  {
    return UPGRADE_STATE_INVALID;
  }
  uint8_t barrier = 0U;
  if (!read(context, UPGRADE_STATE_BARRIER_OFFSET, &barrier, 1U))
  {
    return UPGRADE_STATE_IO_ERROR;
  }
  // 仅屏障为空白才表示没有活动恢复事务；屏障有效但记录缺失属于损坏。
  if (barrier == 0xFFU)
  {
    return UPGRADE_STATE_EMPTY;
  }
  UpgradeStateResult result = UpgradeRecovery_LoadRecords(read, context, state, NULL);
  return (result == UPGRADE_STATE_EMPTY) ? UPGRADE_STATE_CORRUPT : result;
}

// 将有效包槽号转换为保护位掩码。
static uint32_t SlotMask(uint32_t slot)
{
  return ((slot == 1U) || (slot == 2U)) ? (1U << (slot - 1U)) : 0U;
}

// 读取普通和恢复状态，计算被引用槽及允许写入的候选槽。
UpgradeStateResult UpgradeState_ReadProtection(UpgradeStateRead read, void *context,
                                              UpgradeStateProtection *protection)
{
  if (protection == NULL)
  {
    return UPGRADE_STATE_INVALID;
  }
  memset(protection, 0, sizeof(*protection));
  protection->normal_result = UPGRADE_STATE_INVALID;
  protection->recovery_result = UPGRADE_STATE_INVALID;
  if (read == NULL)
  {
    return UPGRADE_STATE_INVALID;
  }
  protection->normal_result = UpgradeState_Load(read, context, &protection->normal, NULL);
  protection->recovery_result = UpgradeRecovery_Load(read, context, &protection->recovery);
  if (protection->normal_result == UPGRADE_STATE_OK)
  {
    protection->referenced_slots |= SlotMask(protection->normal.confirmed.slot)
                                 | SlotMask(protection->normal.candidate.slot);
  }
  if (protection->recovery_result == UPGRADE_STATE_OK)
  {
    protection->referenced_slots |= SlotMask(protection->recovery.package.slot);
  }
  if ((protection->normal_result == UPGRADE_STATE_IO_ERROR)
      || (protection->recovery_result == UPGRADE_STATE_IO_ERROR))
  {
    return UPGRADE_STATE_IO_ERROR;
  }
  if ((protection->normal_result == UPGRADE_STATE_CORRUPT)
      || (protection->recovery_result == UPGRADE_STATE_CORRUPT))
  {
    return UPGRADE_STATE_CORRUPT;
  }
  if ((protection->normal_result == UPGRADE_STATE_UNSUPPORTED)
      || (protection->recovery_result == UPGRADE_STATE_UNSUPPORTED))
  {
    return UPGRADE_STATE_UNSUPPORTED;
  }
  if ((protection->normal_result == UPGRADE_STATE_OK)
      && (protection->normal.phase == UPGRADE_STATE_CONFIRMED)
      && (protection->recovery_result == UPGRADE_STATE_EMPTY))
  {
    // 只有普通 CONFIRMED 且无恢复屏障时，才开放未被记录引用的槽。
    protection->writable_slots = UPGRADE_STATE_ALL_SLOTS & ~protection->referenced_slots;
  }
  return ((protection->normal_result == UPGRADE_STATE_EMPTY)
          && (protection->recovery_result == UPGRADE_STATE_EMPTY))
       ? UPGRADE_STATE_EMPTY : UPGRADE_STATE_OK;
}
