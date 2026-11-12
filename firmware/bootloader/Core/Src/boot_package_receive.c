#include "boot_package_receive.h"

#include "boot_package_flash.h"
#include "flash_partition.h"
#include "usart.h"

#include <stdio.h>
#include <string.h>

#define BOOT_RECEIVE_PAGE_SIZE 256U
// 只读诊断输出到日志串口，不参与维护帧协议。
static bool Boot_ReceiveReply(const char *message)
{
  return HAL_UART_Transmit(&huart1, (uint8_t *)message,
                           (uint16_t)strlen(message), 100U) == HAL_OK;
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
