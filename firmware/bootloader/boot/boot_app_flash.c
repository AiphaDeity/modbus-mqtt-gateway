#include "boot_app_flash.h"
#include "boot_watchdog.h"

#include <string.h>
#include <stdio.h>

#include "stm32f4xx_hal.h"

#define FLASH_APP_START_ADDRESS  0x08010000U
#define FLASH_APP_END_ADDRESS    0x08080000U
// 同Bank擦除时HAL tick可能停顿，软件上限不能替代IWDG及实板计时。
#define APP_SECTOR_TIMEOUT_MS 5000U
#define APP_PROGRAM_TIMEOUT_MS 10000U

// 检查整个非空字节区间是否位于 App 区，先减后比较以避免溢出。
static bool Boot_AppFlashRangeValid(uint32_t address, uint32_t len)
{
  return (address >= FLASH_APP_START_ADDRESS) && (address < FLASH_APP_END_ADDRESS) &&
         (len != 0U) && (len <= FLASH_APP_END_ADDRESS - address);
}

// 安装器逐次调用以擦除 App 的单个扇区，仅允许 4～7；擦除后复查时限并记录耗时。
bool Boot_AppFlashEraseSector(uint32_t sector)
{
  if ((sector < FLASH_SECTOR_4) || (sector > FLASH_SECTOR_7)
      || Boot_WatchdogFailed())
  {
    return false;
  }

  uint32_t started_ms = HAL_GetTick();
  FLASH_EraseInitTypeDef erase =
  {
    .TypeErase = FLASH_TYPEERASE_SECTORS,
    .Sector = sector,
    .NbSectors = 1U,
    .VoltageRange = FLASH_VOLTAGE_RANGE_3 // 板上 3.3 V 供电
  };
  uint32_t sector_error;

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return false;
  }

  bool cycles_available = Boot_WatchdogEnableCycleCounter();
  uint32_t erase_cycles = DWT->CYCCNT;
  HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &sector_error);
  erase_cycles = (uint32_t)(DWT->CYCCNT - erase_cycles);
  HAL_StatusTypeDef lock_status = HAL_FLASH_Lock();
  FLASH_FlushCaches();
  // 周期数包含同Bank擦除造成的CPU停顿，不用HAL毫秒差冒充墙钟耗时。
  printf("WDG BUDGET erase sector=%lu HAL=%u cycles=%lu cycles_ok=%u hclk_hz=%lu elapsed_ms=%lu\r\n",
         (unsigned long)sector, (unsigned int)status, (unsigned long)erase_cycles,
         (unsigned int)cycles_available, (unsigned long)SystemCoreClock,
         (unsigned long)(HAL_GetTick() - started_ms));
  bool within_deadline = Boot_WatchdogCheck(started_ms, APP_SECTOR_TIMEOUT_MS);
  return (status == HAL_OK) && (lock_status == HAL_OK) && within_deadline;
}

/*
 * 将安装器刷出的明文块按 32 位字写到片内 App，address 为绝对地址，len 单位 B。
 * 只编程、不隐式擦除；完成或中途失败后锁回 Flash 并清理缓存，实际内容由上层继续读回检查。
 */
bool Boot_AppFlashProgram(uint32_t address, const uint8_t *data, uint32_t len)
{
  // 目标和长度须按 4 B 对齐，且整个区间落在 App；上层负责末尾 FF 填充及镜像认证。
  if ((data == NULL) || !Boot_AppFlashRangeValid(address, len) ||
      ((address & 3U) != 0U) || ((len & 3U) != 0U) || Boot_WatchdogFailed())
  {
    return false;
  }

  uint32_t started_ms = HAL_GetTick();
  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return false;
  }

  HAL_StatusTypeDef status = HAL_OK;
  for (uint32_t offset = 0U; offset < len; offset += 4U)
  {
    if (!Boot_WatchdogCheck(started_ms, APP_PROGRAM_TIMEOUT_MS))
    {
      status = HAL_TIMEOUT;
      break;
    }
    // 从字节缓冲取一个字，避免要求源指针也按 4 字节对齐。
    uint32_t word;
    memcpy(&word, &data[offset], sizeof(word));
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + offset, word);
    if (status != HAL_OK)
    {
      printf("App program failed address=0x%08lX HAL=%u error=0x%08lX\r\n",
             (unsigned long)(address + offset), (unsigned int)status,
             (unsigned long)HAL_FLASH_GetError());
      break;
    }
  }

  HAL_StatusTypeDef lock_status = HAL_FLASH_Lock();
  // 即使中途写入失败，也可能已有内容变化，立即读回前清除旧缓存。
  FLASH_FlushCaches();
  bool within_deadline = Boot_WatchdogCheck(started_ms, APP_PROGRAM_TIMEOUT_MS);
  return (status == HAL_OK) && (lock_status == HAL_OK) && within_deadline;
}

// 同步逐字节比较本次片内写入与输入块，区别于包验签以及安装结束时的整镜像 SHA-256 检查。
bool Boot_AppFlashVerify(uint32_t address, const uint8_t *data, uint32_t len)
{
  if ((data == NULL) || !Boot_AppFlashRangeValid(address, len) || Boot_WatchdogFailed())
  {
    return false;
  }

  const volatile uint8_t *flash = (const volatile uint8_t *)address;
  for (uint32_t offset = 0U; offset < len; offset++)
  {
    if (flash[offset] != data[offset])
    {
      return false;
    }
  }

  return true;
}

// 从允许的 App 区间同步复制数据到输出缓冲。
bool Boot_AppFlashRead(uint32_t address, uint8_t *data, uint32_t len)
{
  if ((data == NULL) || (len > 256U) || !Boot_AppFlashRangeValid(address, len))
  {
    return false;
  }
  const volatile uint8_t *flash = (const volatile uint8_t *)address;
  for (uint32_t i = 0U; i < len; i++)
  {
    data[i] = flash[i];
  }
  return true;
}
