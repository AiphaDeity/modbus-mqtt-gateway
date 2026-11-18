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

/*
 * 在有效恢复屏障下保存独立恢复记录，校验阶段转换并交替写入日志双副本。
 * 新 sequence 编入持久记录，不回写 const 入参；后续交接或确认须重新加载最新身份。
 */
Boot_UpgradeResult Boot_RecoverySave(const Boot_RecoveryState *state)
{
  if ((state == NULL) || !UpgradeRecovery_RecordValid(state))
  {
    return BOOT_UPGRADE_INVALID;
  }
  // 屏障必须已经有效，不能单独写一条恢复记录就假设设备进入了恢复模式。
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
  // 先更新另一副本正文，最后写独立提交标记；完整提交前仍以原有效副本判断恢复进展。
  if ((Boot_FlashRecoveryJournalErase(address) != HAL_OK)
      || (Boot_FlashRecoveryJournalProgram(address, data, sizeof(data)) != HAL_OK)
      || (Boot_FlashRecoveryJournalProgram(address + COMMIT_OFFSET, commit, sizeof(commit)) != HAL_OK))
  {
    return BOOT_UPGRADE_IO_ERROR;
  }
  // 写命令成功后重新读取并比较所有身份字段，不能只依据 HAL 返回值接受新阶段。
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

/*
 * 串口 BEGIN 的持久准备步骤：保护可信旧源、预留新事务号、写启动屏障，再保存 RECEIVING。
 * 仅建立指定槽的恢复写入资格，不在这里擦包槽；实际擦除由外层接收流程执行。
 */
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
  // 1. 读取普通记录，拒绝读失败/未知版本，并保护已有 confirmed 槽。
  // allow_unknown 只允许显式处理损坏元数据，不能绕过 I/O 错误或可信旧包保护。
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
  // 2. 直接查旧恢复记录，即使先前屏障已清，也不能覆盖尚未被另一可信旧包替代的恢复源。
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
  // 3. 指定槽连基本读取都失败时停止，不通过擦除来绕过读取故障。
  uint32_t slot_address = (slot == 1U) ? FLASH_PART_FW_A_BASE : FLASH_PART_FW_B_BASE;
  uint8_t probe[16];
  if (Boot_FlashRead(slot_address, probe, sizeof(probe)) != HAL_OK)
  {
    return BOOT_UPGRADE_IO_ERROR;
  }
  // 4. 先持久预留新事务号；即使后续建立屏障失败，这个编号也不能回收复用。
  next.operation = UPGRADE_OPERATION_RECOVERY;
  Boot_UpgradeResult reserved = Boot_UpgradeReserveTransaction(&next.tx_id);
  if (reserved != BOOT_UPGRADE_OK)
  {
    return reserved;
  }
  // 5. 屏障先落盘，后面即使修复日志中断，也不能把空白记录当作可正常启动。
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
  // 6. 保存新 RECEIVING 记录，外层重新 Load 取得事务号后，才允许擦槽并发布 RAM 会话。
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
