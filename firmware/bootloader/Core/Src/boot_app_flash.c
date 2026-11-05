#include "boot_app_flash.h"

#include <string.h>

#include "stm32f4xx_hal.h"

#define FLASH_APP_START_ADDRESS  0x08010000U
#define FLASH_APP_END_ADDRESS    0x08080000U

// 检查整个非空字节区间是否位于 App 区，先减后比较以避免溢出。
static bool Boot_AppFlashRangeValid(uint32_t address, uint32_t len)
{
  return (address >= FLASH_APP_START_ADDRESS) && (address < FLASH_APP_END_ADDRESS) &&
         (len != 0U) && (len <= FLASH_APP_END_ADDRESS - address);
}

bool Boot_AppFlashEraseSector(uint32_t sector)
{
  if ((sector < FLASH_SECTOR_4) || (sector > FLASH_SECTOR_7))
  {
    return false;
  }

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

  HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &sector_error);
  HAL_StatusTypeDef lock_status = HAL_FLASH_Lock();
  return (status == HAL_OK) && (lock_status == HAL_OK);
}

bool Boot_AppFlashProgram(uint32_t address, const uint8_t *data, uint32_t len)
{
  if ((data == NULL) || !Boot_AppFlashRangeValid(address, len) ||
      ((address & 3U) != 0U) || ((len & 3U) != 0U))
  {
    return false;
  }

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return false;
  }

  HAL_StatusTypeDef status = HAL_OK;
  for (uint32_t offset = 0U; offset < len; offset += 4U)
  {
    // 从字节缓冲取一个字，避免要求源指针也按 4 字节对齐。
    uint32_t word;
    memcpy(&word, &data[offset], sizeof(word));
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + offset, word);
    if (status != HAL_OK)
    {
      break;
    }
  }

  HAL_StatusTypeDef lock_status = HAL_FLASH_Lock();
  // 即使中途写入失败，也可能已有内容变化，立即读回前清除旧缓存。
  FLASH_FlushCaches();
  return (status == HAL_OK) && (lock_status == HAL_OK);
}

bool Boot_AppFlashVerify(uint32_t address, const uint8_t *data, uint32_t len)
{
  if ((data == NULL) || !Boot_AppFlashRangeValid(address, len))
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
