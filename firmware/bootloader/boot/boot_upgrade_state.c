#include "boot_upgrade_state.h"
#include "boot_package_flash.h"
#include "flash_partition.h"
#include "upgrade_transaction.h"
#include "boot_watchdog.h"

#include <stddef.h>

#define RECORD_SIZE UPGRADE_STATE_RECORD_SIZE
#define COMMIT_OFFSET UPGRADE_STATE_COMMIT_OFFSET
#define COMMIT_SIZE UPGRADE_STATE_COMMIT_SIZE
#define JOURNAL_SECTOR_SIZE UPGRADE_STATE_SECTOR_SIZE

/*
 * 普通状态只使用两个 4 KiB 扇区：[0x380000,0x381000)、[0x381000,0x382000)。
 * 它们保存元数据副本 0/1；固件包在独立 A/B 槽，恢复日志、屏障及事务高水位另行管理。
 */
static const uint32_t record_address[2] =
{
  FLASH_PART_UPGLOG_BASE, FLASH_PART_UPGLOG_BASE + JOURNAL_SECTOR_SIZE
};
static const uint8_t commit_marker[COMMIT_SIZE] = UPGRADE_STATE_NORMAL_COMMIT;

// 读取普通状态双副本，并返回选中副本编号。
static Boot_UpgradeResult LoadRecord(Boot_UpgradeState *state, uint32_t *selected)
{
  return UpgradeState_Load(Boot_FlashReadMetadata, NULL, state, selected);
}

// 在事务操作期限内擦除指定事务记录扇区。
static bool EraseTransaction(void *context, uint32_t offset)
{
  uint32_t started_ms = *(const uint32_t *)context;
  return Boot_WatchdogCheck(started_ms, 10000U)
      && (Boot_FlashTransactionErase(FLASH_PART_UPGLOG_BASE + offset) == HAL_OK)
      && Boot_WatchdogCheck(started_ms, 10000U);
}

// 在事务操作期限内读取升级元数据。
static bool ReadTransaction(void *context, uint32_t offset, uint8_t *data, uint32_t size)
{
  uint32_t started_ms = *(const uint32_t *)context;
  return Boot_WatchdogCheck(started_ms, 10000U) && Boot_FlashReadMetadata(NULL, offset, data, size)
      && Boot_WatchdogCheck(started_ms, 10000U);
}

// 在事务操作期限内编程事务记录数据。
static bool ProgramTransaction(void *context, uint32_t offset, const uint8_t *data, uint32_t size)
{
  uint32_t started_ms = *(const uint32_t *)context;
  return (size <= 256U) && Boot_WatchdogCheck(started_ms, 10000U)
      && (Boot_FlashTransactionProgram(FLASH_PART_UPGLOG_BASE + offset, data, (uint16_t)size) == HAL_OK)
      && Boot_WatchdogCheck(started_ms, 10000U);
}

// 为本次升级持久预留事务号，并检查固定总期限。
Boot_UpgradeResult Boot_UpgradeReserveTransaction(uint64_t *tx_id)
{
  uint32_t started_ms = HAL_GetTick();
  const UpgradeTransactionStorage storage = {ReadTransaction, EraseTransaction, ProgramTransaction, &started_ms};
  return UpgradeTransaction_Reserve(&storage, tx_id);
}

// 读取事务号高水位并检查固定总期限。
Boot_UpgradeResult Boot_UpgradeReadTransaction(UpgradeTransactionState *state)
{
  uint32_t started_ms = HAL_GetTick();
  const UpgradeTransactionStorage storage = {ReadTransaction, EraseTransaction, ProgramTransaction, &started_ms};
  return UpgradeTransaction_Load(&storage, state);
}

// 在显式首次配置时初始化空白事务号区。
Boot_UpgradeResult Boot_UpgradeInitializeTransactions(void)
{
  uint32_t started_ms = HAL_GetTick();
  const UpgradeTransactionStorage storage = {ReadTransaction, EraseTransaction, ProgramTransaction, &started_ms};
  return UpgradeTransaction_Initialize(&storage);
}

// 读取最新有效的普通升级状态。
Boot_UpgradeResult Boot_UpgradeLoad(Boot_UpgradeState *state)
{
  if (state == NULL)
  {
    return BOOT_UPGRADE_INVALID;
  }
  uint32_t selected = 0U;
  return LoadRecord(state, &selected);
}

/*
 * 将安装、试运行或确认进展持久写入外部 Flash 双副本日志，不只是修改 RAM 中的 phase。
 * 先校验转换，再更新另一副本并提交、读回；失败由调用方停止后续擦写或交接，不在此自动重试。
 */
Boot_UpgradeResult Boot_UpgradeSave(const Boot_UpgradeState *state)
{
  // 1. 检查新状态本身合法，再读取最近完整提交的旧记录作为转换依据。
  if ((state == NULL) || !UpgradeState_RecordValid(state))
  {
    return BOOT_UPGRADE_INVALID;
  }
  Boot_UpgradeState old;
  uint32_t selected = 0U;
  Boot_UpgradeResult result = LoadRecord(&old, &selected);
  if ((result != BOOT_UPGRADE_OK) && (result != BOOT_UPGRADE_EMPTY))
  {
    return result;
  }
  // 2. 区分首次建档与后续转换，拒绝非法阶段变化或序号耗尽。
  if (((result == BOOT_UPGRADE_EMPTY)
       && (state->phase != BOOT_UPGRADE_CONFIRMED) && (state->phase != BOOT_UPGRADE_READY))
      || ((result == BOOT_UPGRADE_OK) && !UpgradeState_TransitionValid(&old, state))
      || (old.sequence == UINT32_MAX))
  {
    return BOOT_UPGRADE_INVALID;
  }
  // 3. 首次写副本 0；以后 selected^1 更新另一份，保留当前有效副本，不与包槽 A/B 联动。
  uint32_t target = (result == BOOT_UPGRADE_EMPTY) ? 0U : (selected ^ 1U);
  uint8_t data[RECORD_SIZE];
  // 4. 新 sequence 编入待写字节，不回写 const 入参；后续交接须重新 Load 取得保存后的身份。
  if (!UpgradeState_Encode(state, old.sequence + 1U, data))
  {
    return BOOT_UPGRADE_INVALID;
  }
  // 5. 先擦目标扇区，再写 128 B 正文，最后在偏移 256 B 处写 4 B CMIT；原有效副本保持不动。
  // 正文或标记中途掉电且新标记不完整时，Load 会忽略该副本；正文写完本身不代表已经提交。
  if ((Boot_FlashJournalErase(record_address[target]) != HAL_OK)
      || (Boot_FlashJournalProgram(record_address[target], data, RECORD_SIZE) != HAL_OK)
      || (Boot_FlashJournalProgram(record_address[target] + COMMIT_OFFSET,
                                   commit_marker, COMMIT_SIZE) != HAL_OK))
  {
    // 返回错误不等于没有写入；调用方停止后续推进，重新加载持久状态后才能决定下一步。
    return BOOT_UPGRADE_IO_ERROR;
  }
  // 6. HAL 写成功后仍须重新 Load，核对目标副本、新序号及全部身份字段；不能只比较 phase。
  Boot_UpgradeState check;
  uint32_t check_selected = 0U;
  result = LoadRecord(&check, &check_selected);
  if ((result != BOOT_UPGRADE_OK) || (check_selected != target)
      || (check.sequence != old.sequence + 1U) || (check.phase != state->phase)
      || (check.tx_id != state->tx_id) || (check.trial_attempts != state->trial_attempts)
      || (check.operation != state->operation)
      || !UpgradeState_PackageEqual(&check.confirmed, &state->confirmed)
      || !UpgradeState_PackageEqual(&check.candidate, &state->candidate))
  {
    return (result == BOOT_UPGRADE_OK) ? BOOT_UPGRADE_CORRUPT : result;
  }
  return BOOT_UPGRADE_OK;
}

// 检查包槽是否未被有效升级状态引用且允许写入。
bool Boot_UpgradeSlotWritable(uint32_t slot)
{
  if ((slot != 1U) && (slot != 2U))
  {
    return false;
  }
  Boot_UpgradeState state;
  Boot_UpgradeResult result = Boot_UpgradeLoad(&state);
  return (result == BOOT_UPGRADE_EMPTY)
      || ((result == BOOT_UPGRADE_OK) && (state.confirmed.slot != slot)
          && (state.candidate.slot != slot));
}
