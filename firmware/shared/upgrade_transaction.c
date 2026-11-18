#include "upgrade_transaction.h"

#include <string.h>

/*
 * 事务号高水位与升级记录分开持久保存；先写入新高水位，再把编号交给调用方。
 * 后续升级失败也不回收已预留编号，避免重启后旧会话误匹配新事务。
 */
#define RECORD_SIZE 128U
#define COMMIT_OFFSET 256U
#define SECTOR_SIZE 4096U

// 从事务记录中读取小端 32 位整数。
static uint32_t Read32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8U)
       | ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

// 按小端顺序写入事务记录字段。
static void Write32(uint8_t *data, uint32_t value)
{
  for (uint32_t i = 0U; i < 4U; i++)
  {
    data[i] = (uint8_t)(value >> (i * 8U));
  }
}

// 检查缓冲是否全部为指定字节值。
static bool AllValue(const uint8_t *data, uint32_t size, uint8_t value)
{
  for (uint32_t i = 0U; i < size; i++)
  {
    if (data[i] != value)
    {
      return false;
    }
  }
  return true;
}

// 计算事务记录正文的 CRC-32。
static uint32_t Crc(const uint8_t *data)
{
  uint32_t crc = UINT32_MAX;
  for (uint32_t i = 0U; i < 124U; i++)
  {
    crc ^= data[i];
    for (uint32_t bit = 0U; bit < 8U; bit++)
    {
      crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
  }
  return ~crc;
}

// 将副本编号转换为事务区内的相对地址。
static uint32_t Address(uint32_t copy)
{
  return UPGRADE_STATE_TRANSACTION_OFFSET + copy * SECTOR_SIZE;
}

// 读取事务双副本，检查代次和高水位并选出最新记录。
static UpgradeStateResult Load(const UpgradeTransactionStorage *storage,
                                UpgradeTransactionState *state, uint32_t *selected)
{
  memset(state, 0, sizeof(*state));
  UpgradeTransactionState copies[2] = {0};
  bool valid[2] = {false, false};
  bool blank[2] = {false, false};
  uint8_t data[RECORD_SIZE];
  uint8_t marker[4];
  for (uint32_t i = 0U; i < 2U; i++)
  {
    if (!storage->read(storage->context, Address(i), data, sizeof(data))
        || !storage->read(storage->context, Address(i) + COMMIT_OFFSET, marker, sizeof(marker)))
    {
      return UPGRADE_STATE_IO_ERROR;
    }
    blank[i] = AllValue(data, sizeof(data), 0xFFU) && AllValue(marker, sizeof(marker), 0xFFU);
    if (memcmp(marker, "TXMT", sizeof(marker)) != 0)
    {
      continue;
    }
    if (Read32(data + 124U) != Crc(data))
    {
      return UPGRADE_STATE_CORRUPT;
    }
    if (Read32(data + 8U) != 2U)
    {
      return UPGRADE_STATE_UNSUPPORTED;
    }
    if ((memcmp(data, "GWTXID02", 8U) != 0) || !AllValue(data + 24U, 100U, 0U))
    {
      return UPGRADE_STATE_CORRUPT;
    }
    copies[i].generation = Read32(data + 12U);
    copies[i].last_allocated = (uint64_t)Read32(data + 16U)
                            | ((uint64_t)Read32(data + 20U) << 32U);
    if (copies[i].generation == 0U)
    {
      return UPGRADE_STATE_CORRUPT;
    }
    valid[i] = true;
  }
  if (!valid[0] && !valid[1])
  {
    return (blank[0] && blank[1]) ? UPGRADE_STATE_EMPTY : UPGRADE_STATE_CORRUPT;
  }
  if (valid[0] && valid[1])
  {
    if (copies[0].generation == copies[1].generation)
    {
      return UPGRADE_STATE_CORRUPT;
    }
    // 高代次的高水位不能倒退，不能选出一个会复用事务号的“更新”副本。
    uint32_t newer = copies[1].generation > copies[0].generation ? 1U : 0U;
    if (copies[newer].last_allocated < copies[newer ^ 1U].last_allocated)
    {
      return UPGRADE_STATE_CORRUPT;
    }
  }
  *selected = !valid[0] || (valid[1] && (copies[1].generation > copies[0].generation)) ? 1U : 0U;
  *state = copies[*selected];
  return UPGRADE_STATE_OK;
}

// 写入事务正文和提交标记，再读回核对选中副本及高水位。
static UpgradeStateResult Commit(const UpgradeTransactionStorage *storage,
                                  const UpgradeTransactionState *state, uint32_t target, bool erase)
{
  uint8_t data[RECORD_SIZE] = {0};
  memcpy(data, "GWTXID02", 8U);
  Write32(data + 8U, 2U);
  Write32(data + 12U, state->generation);
  Write32(data + 16U, (uint32_t)state->last_allocated);
  Write32(data + 20U, (uint32_t)(state->last_allocated >> 32U));
  Write32(data + 124U, Crc(data));
  // 目标是另一个副本，旧有效副本保留；正文完成后才写独立页的 TXMT 标记。
  if ((erase && !storage->erase(storage->context, Address(target)))
      || !storage->program(storage->context, Address(target), data, sizeof(data))
      || !storage->program(storage->context, Address(target) + COMMIT_OFFSET,
                            (const uint8_t *)"TXMT", 4U))
  {
    return UPGRADE_STATE_IO_ERROR;
  }
  UpgradeTransactionState check;
  uint32_t selected = 0U;
  UpgradeStateResult result = Load(storage, &check, &selected);
  if (result != UPGRADE_STATE_OK)
  {
    return result;
  }
  return (selected == target) && (check.generation == state->generation)
      && (check.last_allocated == state->last_allocated)
       ? UPGRADE_STATE_OK : UPGRADE_STATE_CORRUPT;
}

// 按当前持久基线读取事务号高水位。
UpgradeStateResult UpgradeTransaction_Load(const UpgradeTransactionStorage *storage,
                                            UpgradeTransactionState *state)
{
  if (state == NULL)
  {
    return UPGRADE_STATE_INVALID;
  }
  memset(state, 0, sizeof(*state));
  if ((storage == NULL) || (storage->read == NULL))
  {
    return UPGRADE_STATE_INVALID;
  }
  uint32_t selected = 0U;
  return Load(storage, state, &selected);
}

// 持久递增事务号高水位，读回成功后才交付编号。
UpgradeStateResult UpgradeTransaction_Reserve(const UpgradeTransactionStorage *storage,
                                               uint64_t *tx_id)
{
  if (tx_id == NULL)
  {
    return UPGRADE_STATE_INVALID;
  }
  *tx_id = 0U;
  if ((storage == NULL) || (storage->read == NULL) || (storage->erase == NULL)
      || (storage->program == NULL))
  {
    return UPGRADE_STATE_INVALID;
  }
  UpgradeTransactionState state;
  uint32_t selected = 0U;
  UpgradeStateResult result = Load(storage, &state, &selected);
  if (result != UPGRADE_STATE_OK)
  {
    return result;
  }
  if ((state.generation == UINT32_MAX) || (state.last_allocated == UINT64_MAX))
  {
    return UPGRADE_STATE_INVALID;
  }
  state.generation++;
  state.last_allocated++;
  // selected ^ 1 在两个副本间切换；不得先擦掉唯一有效的当前副本。
  result = Commit(storage, &state, selected ^ 1U, true);
  if (result == UPGRADE_STATE_OK)
  {
    *tx_id = state.last_allocated;
  }
  return result;
}

// 确认升级元数据全为空白后，建立初始事务号记录。
UpgradeStateResult UpgradeTransaction_Initialize(const UpgradeTransactionStorage *storage)
{
  if ((storage == NULL) || (storage->read == NULL) || (storage->program == NULL))
  {
    return UPGRADE_STATE_INVALID;
  }
  uint8_t data[256];
  // 首次配置还须确认状态日志/屏障全空；不能给已有事务却丢了高水位的设备归零。
  for (uint32_t offset = 0U;
       offset < UPGRADE_STATE_METADATA_SIZE; offset += sizeof(data))
  {
    if (!storage->read(storage->context, offset, data, sizeof(data)))
    {
      return UPGRADE_STATE_IO_ERROR;
    }
    if (!AllValue(data, sizeof(data), 0xFFU))
    {
      return UPGRADE_STATE_CORRUPT;
    }
  }
  UpgradeTransactionState initial = {1U, 0U};
  return Commit(storage, &initial, 0U, false);
}
