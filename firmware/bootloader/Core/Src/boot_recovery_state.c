#include "boot_recovery.h"
#include "flash_partition.h"

#include <string.h>

#define RECORD_SIZE UPGRADE_STATE_RECORD_SIZE
#define COMMIT_OFFSET UPGRADE_STATE_COMMIT_OFFSET
#define SECTOR_SIZE UPGRADE_STATE_SECTOR_SIZE
#define LOG_BASE (FLASH_PART_UPGLOG_BASE + BOOT_RECOVERY_LOG_OFFSET)
#define BARRIER_BASE (FLASH_PART_UPGLOG_BASE + BOOT_RECOVERY_BARRIER_OFFSET)

_Static_assert(BARRIER_BASE + SECTOR_SIZE <= FLASH_PART_UPGLOG_END,
               "Recovery journal must stay inside the upgrade log partition");

static const uint8_t commit[UPGRADE_STATE_COMMIT_SIZE] = UPGRADE_STATE_RECOVERY_COMMIT;

// 显式恢复发起前仍查原始记录，保护尚未被普通日志接管的已确认恢复源。
static Boot_UpgradeResult LoadRecords(Boot_RecoveryState *state, uint32_t *selected)
{
  return UpgradeRecovery_LoadRecords(Boot_FlashReadMetadata, NULL, state, selected);
}

// 检查恢复屏障，决定是否允许普通升级写入。
bool Boot_RecoveryNormalWritesAllowed(void)
{
  return UpgradeRecovery_NormalWritesAllowed(Boot_FlashReadMetadata, NULL);
}

// 加载屏障约束下的独立恢复状态。
Boot_UpgradeResult Boot_RecoveryLoad(Boot_RecoveryState *state)
{
  return UpgradeRecovery_Load(Boot_FlashReadMetadata, NULL, state);
}

// 校验恢复状态转换，写入另一副本并提交。
Boot_UpgradeResult Boot_RecoverySave(const Boot_RecoveryState *state)
{
  if ((state == NULL) || !UpgradeRecovery_RecordValid(state))
  {
    return BOOT_UPGRADE_INVALID;
  }
  uint8_t barrier = 0U;
  if (Boot_FlashRead(BARRIER_BASE, &barrier, 1U) != HAL_OK)
  {
    return BOOT_UPGRADE_IO_ERROR;
  }
  if (barrier == 0xFFU)
  {
    return BOOT_UPGRADE_INVALID;
  }
  Boot_RecoveryState old;
  uint32_t selected = 0U;
  Boot_UpgradeResult result = LoadRecords(&old, &selected);
  if ((result != BOOT_UPGRADE_OK) && (result != BOOT_UPGRADE_EMPTY))
  {
    return result;
  }
  if (((result == BOOT_UPGRADE_EMPTY) && (state->phase != BOOT_RECOVERY_RECEIVING))
      || ((result == BOOT_UPGRADE_OK) && !UpgradeRecovery_TransitionValid(&old, state)))
  {
    return BOOT_UPGRADE_INVALID;
  }
  if (old.sequence == UINT32_MAX)
  {
    return BOOT_UPGRADE_INVALID;
  }
  uint32_t target = result == BOOT_UPGRADE_EMPTY ? 0U : (selected ^ 1U);
  uint32_t address = LOG_BASE + target * SECTOR_SIZE;
  uint8_t data[RECORD_SIZE];
  if (!UpgradeRecovery_Encode(state, old.sequence + 1U, data))
  {
    return BOOT_UPGRADE_INVALID;
  }
  if ((Boot_FlashRecoveryJournalErase(address) != HAL_OK)
      || (Boot_FlashRecoveryJournalProgram(address, data, sizeof(data)) != HAL_OK)
      || (Boot_FlashRecoveryJournalProgram(address + COMMIT_OFFSET, commit, sizeof(commit)) != HAL_OK))
  {
    return BOOT_UPGRADE_IO_ERROR;
  }
  Boot_RecoveryState check;
  uint32_t check_selected = 0U;
  result = LoadRecords(&check, &check_selected);
  if ((result != BOOT_UPGRADE_OK) || (check_selected != target)
      || (check.sequence != old.sequence + 1U)
      || (check.phase != state->phase) || (check.package.slot != state->package.slot)
      || (check.tx_id != state->tx_id) || (check.trial_attempts != state->trial_attempts)
      || (check.operation != state->operation)
      || (check.package.size != state->package.size)
      || (memcmp(check.package.digest, state->package.digest, 32U) != 0))
  {
    return (result == BOOT_UPGRADE_OK) ? BOOT_UPGRADE_CORRUPT : result;
  }
  return BOOT_UPGRADE_OK;
}

// 保护已有可信源后建立屏障和新的恢复接收事务。
Boot_UpgradeResult Boot_RecoveryBegin(uint32_t slot, uint32_t package_size, bool allow_unknown)
{
  Boot_RecoveryState next = {0};
  next.phase = BOOT_RECOVERY_RECEIVING;
  next.package.slot = slot;
  next.package.size = package_size;
  if (((slot != 1U) && (slot != 2U)) || (package_size < 216U)
      || (package_size > 448U * 1024U + 208U))
  {
    return BOOT_UPGRADE_INVALID;
  }
  Boot_UpgradeState normal;
  Boot_UpgradeResult normal_result = Boot_UpgradeLoad(&normal);
  if ((normal_result == BOOT_UPGRADE_IO_ERROR) || (normal_result == BOOT_UPGRADE_UNSUPPORTED)
      || ((normal_result == BOOT_UPGRADE_CORRUPT) && !allow_unknown))
  {
    return normal_result;
  }
  if ((normal_result == BOOT_UPGRADE_OK) && (normal.confirmed.slot == slot))
  {
    return BOOT_UPGRADE_INVALID;
  }
  Boot_RecoveryState previous;
  uint32_t selected = 0U;
  Boot_UpgradeResult result = LoadRecords(&previous, &selected);
  if ((result == BOOT_UPGRADE_IO_ERROR) || (result == BOOT_UPGRADE_UNSUPPORTED)
      || ((result == BOOT_UPGRADE_CORRUPT) && !allow_unknown))
  {
    return result;
  }
  if ((result == BOOT_UPGRADE_OK)
      && ((previous.phase == BOOT_RECOVERY_CONFIRMED) || (previous.phase == BOOT_RECOVERY_DONE))
      && (previous.package.slot == slot)
      && !((normal_result == BOOT_UPGRADE_OK) && (normal.confirmed.slot != 0U)
           && (normal.confirmed.slot != slot)))
  {
    // 没有另一份可信旧包时，不能覆盖唯一已确认的恢复源；仍可显式选择另一槽重新恢复。
    return BOOT_UPGRADE_INVALID;
  }
  // 指定槽连基本读取都失败时停止，不通过擦除来绕过读取故障。
  uint32_t slot_address = (slot == 1U) ? FLASH_PART_FW_A_BASE : FLASH_PART_FW_B_BASE;
  uint8_t probe[16];
  if (Boot_FlashRead(slot_address, probe, sizeof(probe)) != HAL_OK)
  {
    return BOOT_UPGRADE_IO_ERROR;
  }
#if UPGRADE_PERSISTENCE_VERSION == 2
  next.operation = UPGRADE_OPERATION_RECOVERY;
  Boot_UpgradeResult reserved = Boot_UpgradeReserveTransaction(&next.tx_id);
  if (reserved != BOOT_UPGRADE_OK)
  {
    return reserved;
  }
#endif
  // 屏障先落盘，后面即使修复日志中断，也不能把空白记录当作可正常启动。
  const uint8_t active = 0U;
  if (Boot_FlashRecoveryJournalProgram(BARRIER_BASE, &active, 1U) != HAL_OK)
  {
    return BOOT_UPGRADE_IO_ERROR;
  }
  if (result == BOOT_UPGRADE_CORRUPT)
  {
    if ((Boot_FlashRecoveryJournalErase(LOG_BASE) != HAL_OK)
        || (Boot_FlashRecoveryJournalErase(LOG_BASE + SECTOR_SIZE) != HAL_OK))
    {
      return BOOT_UPGRADE_IO_ERROR;
    }
  }
  return Boot_RecoverySave(&next);
}

// 检查写区间是否落在当前恢复接收事务指定的包槽。
bool Boot_RecoveryWriteAllowed(uint32_t address, uint32_t size)
{
  Boot_RecoveryState state;
  if ((Boot_RecoveryLoad(&state) != BOOT_UPGRADE_OK) || (state.phase != BOOT_RECOVERY_RECEIVING)
      || (size == 0U))
  {
    return false;
  }
  Boot_UpgradeState normal;
  Boot_UpgradeResult result = Boot_UpgradeLoad(&normal);
  if ((result == BOOT_UPGRADE_IO_ERROR) || (result == BOOT_UPGRADE_UNSUPPORTED)
      || ((result == BOOT_UPGRADE_OK) && (normal.confirmed.slot == state.package.slot)))
  {
    return false;
  }
  uint32_t start = (state.package.slot == 1U) ? FLASH_PART_FW_A_BASE : FLASH_PART_FW_B_BASE;
  uint32_t end = (state.package.slot == 1U) ? FLASH_PART_FW_A_END : FLASH_PART_FW_B_END;
  return (address >= start) && (address < end) && (size <= end - address);
}
