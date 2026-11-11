#include "boot_package_receive.h"

#include "boot_package_flash.h"
#include "boot_upgrade_state.h"
#include "boot_install.h"
#include "boot_recovery.h"
#include "boot_watchdog.h"
#include "flash_partition.h"
#include "usart.h"

#include <stdio.h>
#include <string.h>

#define BOOT_RECEIVE_PAGE_SIZE 256U
#define BOOT_RECEIVE_MAX_SIZE (448U * 1024U + 208U)
#define BOOT_RECEIVE_PAGE_TIMEOUT_MS 2000U
#define BOOT_RECEIVE_TOTAL_TIMEOUT_MS 180000U
#define BOOT_RECEIVE_REQUEST_TIMEOUT_MS 10000U
#define BOOT_RECEIVE_REQUEST_SLICE_MS 250U
#define BOOT_RECEIVE_SCAN_TIMEOUT_MS 180000U

// 协议响应使用独立行和固定前缀，与普通启动日志区分。
static bool Boot_ReceiveReply(const char *message)
{
  return HAL_UART_Transmit(&huart1, (uint8_t *)message,
                           (uint16_t)strlen(message), 100U) == HAL_OK;
}

// 只读检查整个槽；任何已有数据或读取错误都禁止本次写入。
static bool Boot_ReceiveSlotBlank(uint32_t start, uint32_t end)
{
  uint32_t started_ms = HAL_GetTick();
  uint8_t data[BOOT_RECEIVE_PAGE_SIZE];
  char reply[112];
  char slot = (start == FLASH_PART_FW_A_BASE) ? 'A' : 'B';
  for (uint32_t address = start; address < end; address += sizeof(data))
  {
    if (!Boot_WatchdogCheck(started_ms, BOOT_RECEIVE_SCAN_TIMEOUT_MS))
    {
      (void)Boot_ReceiveReply("PKG SCAN_TIMEOUT\r\n");
      return false;
    }
    HAL_StatusTypeDef status = Boot_FlashRead(address, data, sizeof(data));
    if (status != HAL_OK)
    {
      (void)snprintf(reply, sizeof(reply),
                     "Slot %c: read failed, address=0x%08lX, HAL status=%u\r\n",
                     slot, (unsigned long)address, (unsigned int)status);
      (void)Boot_ReceiveReply(reply);
      return false;
    }
    for (size_t i = 0U; i < sizeof(data); i++)
    {
      if (data[i] != 0xFFU)
      {
        (void)snprintf(reply, sizeof(reply),
                       "Slot %c: nonblank, address=0x%08lX, value=0x%02X\r\n",
                       slot, (unsigned long)(address + i), (unsigned int)data[i]);
        (void)Boot_ReceiveReply(reply);
        return false;
      }
    }
    if (!Boot_WatchdogProgress(started_ms, BOOT_RECEIVE_SCAN_TIMEOUT_MS))
    {
      (void)Boot_ReceiveReply("PKG SCAN_TIMEOUT\r\n");
      return false;
    }
  }
  (void)snprintf(reply, sizeof(reply), "Slot %c: blank\r\n", slot);
  (void)Boot_ReceiveReply(reply);
  return true;
}

/*
 * 每个固定位置读 32 次、每次 256 字节，与首次成功读回的数据逐字节比较。
 * 每个位置最多打印 3 次差异详情，其余差异计入汇总，避免日志淹没结果。
 * 对接收缓冲交替填充 00/FF，便于暴露未完整更新缓冲等问题；不向 Flash 编程。
 */
void Boot_TestFlashReadDiagnostic(void)
{
  static const uint32_t addresses[] =
  {
    0U, FLASH_PART_FW_A_BASE, FLASH_PART_FW_A_BASE + 0x100U,
    FLASH_PART_FW_B_BASE, FLASH_PART_FW_B_BASE + 0x1500U
  };
  uint8_t reference[BOOT_RECEIVE_PAGE_SIZE];
  uint8_t data[BOOT_RECEIVE_PAGE_SIZE];
  char reply[144];
  uint32_t total_errors = 0U;
  uint32_t total_mismatches = 0U;

  (void)Boot_ReceiveReply("\r\nREADTEST BEGIN: read-only, 5 locations x 32 reads x 256 bytes\r\n");
  for (size_t location = 0U; location < sizeof(addresses) / sizeof(addresses[0]); location++)
  {
    uint32_t errors = 0U;
    uint32_t mismatches = 0U;
    uint32_t successful = 0U;
    uint32_t nonblank = 0U;
    bool have_reference = false;
    for (uint32_t round = 0U; round < 32U; round++)
    {
      memset(data, (round & 1U) ? 0xFF : 0x00, sizeof(data));
      HAL_StatusTypeDef status = Boot_FlashRead(addresses[location], data, sizeof(data));
      if (status != HAL_OK)
      {
        errors++;
        if (errors <= 3U)
        {
          (void)snprintf(reply, sizeof(reply),
                         "READTEST ERROR: base=0x%08lX round=%lu HAL=%u\r\n",
                         (unsigned long)addresses[location], (unsigned long)(round + 1U),
                         (unsigned int)status);
          (void)Boot_ReceiveReply(reply);
        }
        continue;
      }
      successful++;
      if (!have_reference)
      {
        memcpy(reference, data, sizeof(reference));
        have_reference = true;
        for (size_t i = 0U; i < sizeof(reference); i++)
        {
          if (reference[i] != 0xFFU)
          {
            nonblank++;
          }
        }
        continue;
      }
      for (size_t i = 0U; i < sizeof(data); i++)
      {
        if (data[i] != reference[i])
        {
          mismatches++;
          if (mismatches <= 3U)
          {
            (void)snprintf(reply, sizeof(reply),
                           "READTEST DIFF: address=0x%08lX round=%lu first=0x%02X now=0x%02X\r\n",
                           (unsigned long)(addresses[location] + i), (unsigned long)(round + 1U),
                           (unsigned int)reference[i], (unsigned int)data[i]);
            (void)Boot_ReceiveReply(reply);
          }
          break;
        }
      }
    }
    (void)snprintf(reply, sizeof(reply),
                   "READTEST SUMMARY: base=0x%08lX reads=%lu/32 errors=%lu mismatch_reads=%lu reference_nonFF=%lu\r\n",
                   (unsigned long)addresses[location], (unsigned long)successful,
                   (unsigned long)errors, (unsigned long)mismatches, (unsigned long)nonblank);
    (void)Boot_ReceiveReply(reply);
    total_errors += errors;
    total_mismatches += mismatches;
  }
  (void)snprintf(reply, sizeof(reply),
                 "READTEST END: %s errors=%lu mismatch_reads=%lu; no writes, no App jump\r\n",
                 ((total_errors == 0U) && (total_mismatches == 0U)) ? "CONSISTENT" : "UNSTABLE",
                 (unsigned long)total_errors, (unsigned long)total_mismatches);
  (void)Boot_ReceiveReply(reply);
}


/*
 * 返回整包剩余毫秒数；到期时报告已写入并读回的字节数，返回0。
 * 使用HAL单调时基，无符号减法允许计数跨一次32位回绕。
 */
static uint32_t Boot_ReceiveRemainingMs(uint32_t started_ms,
                                       uint32_t written,
                                       const char *prefix)
{
  uint32_t elapsed_ms = (uint32_t)(HAL_GetTick() - started_ms);
  if (elapsed_ms < BOOT_RECEIVE_TOTAL_TIMEOUT_MS)
  {
    return BOOT_RECEIVE_TOTAL_TIMEOUT_MS - elapsed_ms;
  }

  // 整包总期限到达，锁存本次监督失败。
  (void)Boot_WatchdogCheck(started_ms, BOOT_RECEIVE_TOTAL_TIMEOUT_MS);
  char reply[112];
  (void)snprintf(reply, sizeof(reply),
                 "%s TOTAL_TIMEOUT written=%lu elapsed_ms=%lu limit_ms=%lu\r\n",
                 prefix, (unsigned long)written, (unsigned long)elapsed_ms,
                 (unsigned long)BOOT_RECEIVE_TOTAL_TIMEOUT_MS);
  (void)Boot_ReceiveReply(reply);
  return 0U;
}

/*
 * 同步接收并逐页写入；整包软件期限从本函数开始，不包含此前擦槽。
 * 期限在阻塞调用前后检查，不能替代CPU/时基停止时的硬件看门狗。
 * 失败保留半包且不登记候选，调用方须停止本次接收流程。
 */
bool Boot_ReceivePackageData(uint32_t address, uint32_t total_size, bool recovery)
{
  if (((address != FLASH_PART_FW_A_BASE) && (address != FLASH_PART_FW_B_BASE))
      || (total_size < 209U) || (total_size > BOOT_RECEIVE_MAX_SIZE))
  {
    return false;
  }

  if (Boot_WatchdogFailed())
  {
    return false;
  }

  uint8_t data[BOOT_RECEIVE_PAGE_SIZE];
  char reply[112];
  const char *prefix = recovery ? "RCV" : "PKG";
  uint32_t started_ms = HAL_GetTick();
  uint32_t offset = 0U;

  while (offset < total_size)
  {
    uint32_t remaining_ms = Boot_ReceiveRemainingMs(started_ms, offset, prefix);
    if (remaining_ms == 0U)
    {
      return false;
    }

    uint32_t page_timeout_ms = (remaining_ms < BOOT_RECEIVE_PAGE_TIMEOUT_MS)
                             ? remaining_ms : BOOT_RECEIVE_PAGE_TIMEOUT_MS;
    uint16_t size = (total_size - offset > sizeof(data))
                  ? (uint16_t)sizeof(data) : (uint16_t)(total_size - offset);

    HAL_StatusTypeDef status = HAL_UART_Receive(&huart1, data, size, page_timeout_ms);
    if (status != HAL_OK)
    {
      if ((status == HAL_TIMEOUT)
          && (Boot_ReceiveRemainingMs(started_ms, offset, prefix) == 0U))
      {
        return false;
      }

      (void)snprintf(reply, sizeof(reply), "%s RX_FAILED offset=%lu HAL=%u\r\n",
                     prefix, (unsigned long)offset, (unsigned int)status);
      (void)Boot_ReceiveReply(reply);
      return false;
    }

    // UART返回成功仍须检查整包期限，迟到的数据不能继续写入。
    if (Boot_ReceiveRemainingMs(started_ms, offset, prefix) == 0U)
    {
      return false;
    }

    // 收齐一页是实际进展，将最长2s接收与后续Flash处理分开监督，不延长整包期限。
    if (!Boot_WatchdogProgress(started_ms, BOOT_RECEIVE_TOTAL_TIMEOUT_MS))
    {
      return false;
    }
    status = recovery ? Boot_FlashRecoveryProgram(address + offset, data, size)
                      : Boot_FlashProgramPage(address + offset, data, size);
    if (status != HAL_OK)
    {
      (void)snprintf(reply, sizeof(reply),
                     "%s WRITE_FAILED address=%08lX HAL=%u\r\n",
                     prefix, (unsigned long)(address + offset),
                     (unsigned int)status);
      (void)Boot_ReceiveReply(reply);
      return false;
    }

    offset += size; // 驱动成功已包含读回检查。
    if (Boot_ReceiveRemainingMs(started_ms, offset, prefix) == 0U)
    {
      return false;
    }

    (void)snprintf(reply, sizeof(reply), "%s ACK %lu\r\n",
                   prefix, (unsigned long)offset);
    if (!Boot_ReceiveReply(reply)
        || (Boot_ReceiveRemainingMs(started_ms, offset, prefix) == 0U)
        || !Boot_WatchdogProgress(started_ms, BOOT_RECEIVE_TOTAL_TIMEOUT_MS))
    {
      return false;
    }
  }
  // 最后一条ACK也可能耗时，到期后不能报告整包接收成功。
  return Boot_ReceiveRemainingMs(started_ms, offset, prefix) != 0U;
}

/*
 * 收齐一个请求头，总等待时间不因收到字节而延长。
 * 以单字节接收保留部分请求，避免对整头重试时丢失HAL已经收到的前缀。
 * received返回实际接收字节数，用于区分正常空闲和半截请求。
 */
static HAL_StatusTypeDef Boot_ReceiveRequest(uint8_t *request,
                                             uint16_t size,
                                             uint16_t *received)
{
  uint32_t started_ms = HAL_GetTick();
  *received = 0U;

  while (*received < size)
  {
    if (Boot_WatchdogFailed())
    {
      return HAL_ERROR;
    }

    uint32_t elapsed_ms = (uint32_t)(HAL_GetTick() - started_ms);
    if (elapsed_ms >= BOOT_RECEIVE_REQUEST_TIMEOUT_MS)
    {
      // 正常维护窗口结束，不锁存监督故障。
      return HAL_TIMEOUT;
    }

    uint32_t remaining_ms = BOOT_RECEIVE_REQUEST_TIMEOUT_MS - elapsed_ms;
    uint32_t wait_ms = (remaining_ms < BOOT_RECEIVE_REQUEST_SLICE_MS)
                     ? remaining_ms : BOOT_RECEIVE_REQUEST_SLICE_MS;

    uint32_t before_ms = HAL_GetTick();
    HAL_StatusTypeDef status = HAL_UART_Receive(&huart1, request + *received,
                                                1U, wait_ms);
    if ((status != HAL_OK) && (status != HAL_TIMEOUT))
    {
      return status;
    }
    if (status == HAL_OK)
    {
      (*received)++;
    }
    else if (HAL_GetTick() == before_ms)
    {
      // 非零等待却立即超时，不能当作正常等待完成而喂狗。
      return HAL_ERROR;
    }

    if ((uint32_t)(HAL_GetTick() - started_ms) >= BOOT_RECEIVE_REQUEST_TIMEOUT_MS)
    {
      return HAL_TIMEOUT;
    }

    // 正常等待结束不是监督故障；固定窗口由上面的判断控制。
    if (!Boot_WatchdogIdleProgress())
    {
      return HAL_ERROR;
    }
  }

  return HAL_OK;
}

// 处理一次 Boot 串口请求，收包认证后登记或转交恢复服务。
BootReceiveResult Boot_ReceivePackage(const uint8_t *public_key, const uint8_t *aes_key)
{
  uint8_t request[12];
  Boot_PackageFlashSource source = {0U, 0U};
  uint32_t firmware_size = 0U;

  if (!Boot_ReceiveReply("\r\nPKG WAIT\r\n"))
  {
    return BOOT_RECEIVE_NO_REQUEST;
  }
  uint16_t received = 0U;
  HAL_StatusTypeDef status = Boot_ReceiveRequest(request, sizeof(request), &received);
  if (status != HAL_OK)
  {
    if ((status == HAL_TIMEOUT) && (received == 0U))
    {
      (void)Boot_ReceiveReply("PKG NO_REQUEST\r\n");
      return BOOT_RECEIVE_NO_REQUEST;
    }

    char reply[96];
    (void)snprintf(reply, sizeof(reply),
                   "PKG REQUEST_FAILED received=%u/12 HAL=%u\r\n",
                   (unsigned int)received, (unsigned int)status);
    (void)Boot_ReceiveReply(reply);
    return BOOT_RECEIVE_COMMAND;
  }
  if ((memcmp(request, "GWRC", 4U) == 0) || (memcmp(request, "GWRTINIT", 8U) == 0))
  {
    return Boot_RecoveryHandleRequest(request, public_key, aes_key);
  }
  if ((public_key == NULL) || (aes_key == NULL))
  {
    (void)Boot_ReceiveReply("PKG KEYS_NOT_READY\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  if (!Boot_RecoveryNormalWritesAllowed())
  {
    (void)Boot_ReceiveReply("PKG RECOVERY_ACTIVE\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  bool enroll = memcmp(request, "GWBASE01", 8U) == 0;
  if (!enroll && (memcmp(request, "GWLOAD01", 8U) != 0))
  {
    (void)Boot_ReceiveReply("PKG BAD_REQUEST\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  source.size = (uint32_t)request[8] | ((uint32_t)request[9] << 8U)
              | ((uint32_t)request[10] << 16U) | ((uint32_t)request[11] << 24U);
  if ((source.size < 209U) || (source.size > BOOT_RECEIVE_MAX_SIZE))
  {
    (void)Boot_ReceiveReply("PKG BAD_SIZE\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  // 已有候选/安装事务时先拒绝，不能先占用另一个空槽再发现状态冲突。
  Boot_UpgradeState initial_state;
  Boot_UpgradeResult initial_result = Boot_UpgradeLoad(&initial_state);
  if ((enroll && (initial_result != BOOT_UPGRADE_EMPTY))
      || (!enroll && ((initial_result != BOOT_UPGRADE_OK)
                      || (initial_state.phase != BOOT_UPGRADE_CONFIRMED))))
  {
    (void)Boot_ReceiveReply("PKG STATE_REJECTED\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  if (Boot_UpgradeSlotWritable(1U) && Boot_ReceiveSlotBlank(FLASH_PART_FW_A_BASE, FLASH_PART_FW_A_END))
  {
    source.address = FLASH_PART_FW_A_BASE;
  }
  else if (Boot_UpgradeSlotWritable(2U) && Boot_ReceiveSlotBlank(FLASH_PART_FW_B_BASE, FLASH_PART_FW_B_END))
  {
    source.address = FLASH_PART_FW_B_BASE;
  }
  else
  {
    (void)Boot_ReceiveReply("PKG NO_BLANK_SLOT\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  uint64_t tx_id = 0U;
  if (Boot_UpgradeReserveTransaction(&tx_id) != BOOT_UPGRADE_OK)
  {
    (void)Boot_ReceiveReply("PKG TX_NOT_READY\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  char reply[80];
  (void)snprintf(reply, sizeof(reply), "PKG READY %08lX\r\n", (unsigned long)source.address);
  if (!Boot_ReceiveReply(reply))
  {
    return BOOT_RECEIVE_COMMAND;
  }
  if (!Boot_ReceivePackageData(source.address, source.size, false))
  {
    return BOOT_RECEIVE_COMMAND;
  }
  if (!Boot_ReceiveReply("PKG VERIFYING\r\n"))
  {
    return BOOT_RECEIVE_COMMAND;
  }
  if (!Boot_PackageVerifyFlash(&source, public_key, aes_key, &firmware_size))
  {
    (void)Boot_ReceiveReply("PKG REJECTED\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  if (enroll)
  {
    Boot_InstallResult result = Boot_InstallEnroll(&source, public_key, aes_key, tx_id);
    if (result != BOOT_INSTALL_BOOT_EXISTING)
    {
      (void)snprintf(reply, sizeof(reply), "PKG BASELINE_FAILED %u\r\n", (unsigned int)result);
      (void)Boot_ReceiveReply(reply);
      return BOOT_RECEIVE_COMMAND;
    }
    (void)snprintf(reply, sizeof(reply), "PKG ENROLLED %lu %08lX\r\n",
                   (unsigned long)firmware_size, (unsigned long)source.address);
    (void)Boot_ReceiveReply(reply);
    return BOOT_RECEIVE_PACKAGE;
  }
  // 只有整包认证通过才登记候选；普通候选要求已有可信恢复包。
  Boot_UpgradeState state;
  Boot_UpgradeResult load_result = Boot_UpgradeLoad(&state);
  if (((load_result != BOOT_UPGRADE_EMPTY) && (load_result != BOOT_UPGRADE_OK))
      || ((load_result == BOOT_UPGRADE_OK) && (state.phase != BOOT_UPGRADE_CONFIRMED)))
  {
    (void)Boot_ReceiveReply("PKG STATE_REJECTED\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  uint8_t manifest[128];
  if (Boot_FlashRead(source.address, manifest, sizeof(manifest)) != HAL_OK)
  {
    (void)Boot_ReceiveReply("PKG STATE_READ_FAILED\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  state.phase = BOOT_UPGRADE_READY;
  state.tx_id = tx_id;
  state.trial_attempts = 0U;
  state.operation = UPGRADE_OPERATION_INSTALL;
  state.candidate.slot = (source.address == FLASH_PART_FW_A_BASE) ? 1U : 2U;
  state.candidate.size = source.size;
  memcpy(state.candidate.digest, manifest + 60U, sizeof(state.candidate.digest));
  if (Boot_UpgradeSave(&state) != BOOT_UPGRADE_OK)
  {
    (void)Boot_ReceiveReply("PKG STATE_SAVE_FAILED\r\n");
    return BOOT_RECEIVE_COMMAND;
  }
  (void)snprintf(reply, sizeof(reply), "PKG VERIFIED %lu %08lX\r\n",
                 (unsigned long)firmware_size, (unsigned long)source.address);
  (void)Boot_ReceiveReply(reply);
  return BOOT_RECEIVE_PACKAGE;
}
