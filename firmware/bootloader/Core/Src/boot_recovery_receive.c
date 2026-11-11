#include "boot_recovery.h"
#include "flash_partition.h"
#include "upgrade_transaction.h"
#include "usart.h"
#include "boot_watchdog.h"

#include <stdio.h>
#include <string.h>

// 覆盖整槽擦除和逐扇区读回；固定总上限，不随扇区完成而续期。
#define RECOVERY_ERASE_TIMEOUT_MS 300000U

// 通过 USART1 发送完整恢复应答，失败返回 false。
static bool Reply(const char *message)
{
  return HAL_UART_Transmit(&huart1, (uint8_t *)message, (uint16_t)strlen(message), 100U) == HAL_OK;
}

// 只读输出包槽、持久状态及事务号信息。
static void PrintInfo(void)
{
  char reply[384];
  Boot_UpgradeState normal;
  Boot_RecoveryState recovery;
  Boot_UpgradeResult normal_result = Boot_UpgradeLoad(&normal);
  Boot_UpgradeResult recovery_result = Boot_RecoveryLoad(&recovery);
  UpgradeTransactionState transaction;
  Boot_UpgradeResult transaction_result = Boot_UpgradeReadTransaction(&transaction);
  uint64_t active_tx = recovery_result == BOOT_UPGRADE_OK ? recovery.tx_id : normal.tx_id;
  uint32_t active_attempts = recovery_result == BOOT_UPGRADE_OK
                         ? recovery.trial_attempts : normal.trial_attempts;
  uint32_t protected_slot = (normal_result == BOOT_UPGRADE_OK) ? normal.confirmed.slot : 0U;
  if ((protected_slot == 0U) && (recovery_result == BOOT_UPGRADE_OK)
      && ((recovery.phase == BOOT_RECOVERY_CONFIRMED) || (recovery.phase == BOOT_RECOVERY_DONE)))
  {
    protected_slot = recovery.package.slot;
  }
  const uint32_t addresses[2] = {FLASH_PART_FW_A_BASE, FLASH_PART_FW_B_BASE};
  for (uint32_t i = 0U; i < 2U; i++)
  {
    uint8_t data[16] = {0};
    HAL_StatusTypeDef status = Boot_FlashRead(addresses[i], data, sizeof(data));
    (void)snprintf(reply, sizeof(reply), "RCV SLOT %c address=%08lX HAL=%u first=%02X%02X%02X%02X\r\n",
                   (i == 0U) ? 'A' : 'B', (unsigned long)addresses[i], (unsigned int)status,
                   data[0], data[1], data[2], data[3]);
    (void)Reply(reply);
  }
  (void)snprintf(reply, sizeof(reply),
                 "RCV INFO normal=%u recovery=%u phase=%u sequence=%lu protected=%lu source=%lu compat=%u tx_hi=%08lX tx_lo=%08lX attempts=%lu normal_phase=%u normal_sequence=%lu tx_state=%u tx_generation=%lu tx_high_hi=%08lX tx_high_lo=%08lX\r\n",
                 (unsigned int)normal_result, (unsigned int)recovery_result,
                 (unsigned int)recovery.phase, (unsigned long)recovery.sequence,
                 (unsigned long)protected_slot,
                 (unsigned long)recovery.package.slot, (unsigned int)UPGRADE_PERSISTENCE_VERSION,
                 (unsigned long)(active_tx >> 32U), (unsigned long)active_tx,
                 (unsigned long)active_attempts, (unsigned int)normal.phase, (unsigned long)normal.sequence,
                 (unsigned int)transaction_result, (unsigned long)transaction.generation,
                 (unsigned long)(transaction.last_allocated >> 32U), (unsigned long)transaction.last_allocated);
  (void)Reply(reply);
}

// 解析恢复请求，执行查询、显式配置或恢复收包。
BootReceiveResult Boot_RecoveryHandleRequest(const uint8_t *request,
                                            const uint8_t *public_key, const uint8_t *aes_key)
{
  uint32_t argument = (uint32_t)request[8] | ((uint32_t)request[9] << 8U)
                    | ((uint32_t)request[10] << 16U) | ((uint32_t)request[11] << 24U);
  if (memcmp(request, "GWRCINFO", 8U) == 0)
  {
    PrintInfo();
    return BOOT_RECEIVE_COMMAND;
  }
  if ((memcmp(request, "GWRTINIT", 8U) == 0) && (argument == 0U))
  {
    char reply[64];
    Boot_UpgradeResult result = Boot_UpgradeInitializeTransactions();
    (void)snprintf(reply, sizeof(reply), "RCV TX_INIT result=%u\r\n", (unsigned int)result);
    (void)Reply(reply);
    return BOOT_RECEIVE_COMMAND;
  }
  if ((memcmp(request, "GWRCEXIT", 8U) == 0) || (memcmp(request, "GWRCRUN1", 8U) == 0))
  {
    return BOOT_RECEIVE_EXIT;
  }
  if ((public_key == NULL) || (aes_key == NULL))
  {
    (void)Reply("RCV KEYS_NOT_READY\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  bool force = (memcmp(request, "GWRCFRA1", 8U) == 0) || (memcmp(request, "GWRCFRB1", 8U) == 0);
  bool slot_a = (memcmp(request, "GWRCVRA1", 8U) == 0) || (memcmp(request, "GWRCFRA1", 8U) == 0);
  bool slot_b = (memcmp(request, "GWRCVRB1", 8U) == 0) || (memcmp(request, "GWRCFRB1", 8U) == 0);
  if (!slot_a && !slot_b)
  {
    (void)Reply("RCV BAD_REQUEST\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  uint32_t slot = slot_a ? 1U : 2U;
  Boot_UpgradeResult result = Boot_RecoveryBegin(slot, argument, force);
  char reply[112];
  if (result != BOOT_UPGRADE_OK)
  {
    (void)snprintf(reply, sizeof(reply), "RCV BEGIN_FAILED result=%u slot=%lu\r\n",
                   (unsigned int)result, (unsigned long)slot);
    (void)Reply(reply);
    return BOOT_RECEIVE_COMMAND;
  }
  uint32_t start = slot_a ? FLASH_PART_FW_A_BASE : FLASH_PART_FW_B_BASE;
  uint32_t end = slot_a ? FLASH_PART_FW_A_END : FLASH_PART_FW_B_END;
  (void)snprintf(reply, sizeof(reply), "RCV ERASING slot=%lu start=%08lX end=%08lX\r\n",
                 (unsigned long)slot, (unsigned long)start, (unsigned long)end);
  if (!Reply(reply))
  {
    return BOOT_RECEIVE_COMMAND;
  }
  uint32_t erase_started_ms = HAL_GetTick();
  for (uint32_t address = start; address < end; address += 4096U)
  {
    if (!Boot_WatchdogCheck(erase_started_ms, RECOVERY_ERASE_TIMEOUT_MS))
    {
      (void)Reply("RCV ERASE_TIMEOUT\r\n");
      return BOOT_RECEIVE_COMMAND;
    }
    HAL_StatusTypeDef status = Boot_FlashRecoveryErase(address);
    if (status != HAL_OK)
    {
      (void)snprintf(reply, sizeof(reply), "RCV ERASE_FAILED address=%08lX HAL=%u\r\n",
                     (unsigned long)address, (unsigned int)status);
      (void)Reply(reply);
      return BOOT_RECEIVE_COMMAND;
    }
    if (!Boot_WatchdogProgress(erase_started_ms, RECOVERY_ERASE_TIMEOUT_MS))
    {
      (void)Reply("RCV ERASE_TIMEOUT\r\n");
      return BOOT_RECEIVE_COMMAND;
    }
  }
  (void)snprintf(reply, sizeof(reply), "RCV READY %lu %lu\r\n", (unsigned long)slot, (unsigned long)argument);
  if (!Reply(reply) || !Boot_ReceivePackageData(start, argument, true))
  {
    return BOOT_RECEIVE_COMMAND;
  }
  if (!Reply("RCV VERIFYING\r\n"))
  {
    return BOOT_RECEIVE_COMMAND;
  }
  Boot_UpgradeResult accepted = Boot_RecoveryAccept(public_key, aes_key);
  if (accepted != BOOT_UPGRADE_OK)
  {
    (void)snprintf(reply, sizeof(reply), "RCV REJECTED result=%u\r\n", (unsigned int)accepted);
    (void)Reply(reply);
    return BOOT_RECEIVE_COMMAND;
  }
  (void)snprintf(reply, sizeof(reply), "RCV VERIFIED %lu %lu\r\n",
                 (unsigned long)(argument - 208U), (unsigned long)slot);
  return Reply(reply) ? BOOT_RECEIVE_PACKAGE : BOOT_RECEIVE_COMMAND;
}
