#include "boot_package_flash.h"

#include "spi.h"

#define BOOT_FLASH_CAPACITY (16U * 1024U * 1024U)
#define BOOT_FLASH_READ_MAX 256U

// 在同一次片选期间发读命令并接收数据，所有 HAL 错误均交给调用方处理。
HAL_StatusTypeDef Boot_FlashRead(uint32_t address, uint8_t *data, uint16_t size)
{
  if ((data == NULL) || (size == 0U) || (size > BOOT_FLASH_READ_MAX)
      || (address >= BOOT_FLASH_CAPACITY) || (size > BOOT_FLASH_CAPACITY - address))
  {
    return HAL_ERROR;
  }
  uint8_t command[4] =
  {
    0x03U, (uint8_t)(address >> 16U), (uint8_t)(address >> 8U), (uint8_t)address
  };

  HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_RESET);
  HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi2, command, sizeof(command), 10U);
  if (status == HAL_OK)
  {
    status = HAL_SPI_Receive(&hspi2, data, size, 10U);
  }
  HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_SET);
  return status;
}

// 用减法先检查区间，再计算物理偏移，避免不可信长度触发加法溢出。
bool Boot_PackageReadFlash(void *context, size_t offset, uint8_t *data, size_t size)
{
  const Boot_PackageFlashSource *source = context;
  if ((source == NULL) || (data == NULL) || (size == 0U)
      || (size > BOOT_FLASH_READ_MAX)
      || (source->address >= BOOT_FLASH_CAPACITY)
      || (source->size > BOOT_FLASH_CAPACITY - source->address)
      || (offset > source->size) || (size > source->size - offset))
  {
    return false;
  }
  return Boot_FlashRead(source->address + (uint32_t)offset, data, (uint16_t)size) == HAL_OK;
}

// 保存一份源描述快照，避免回调期间调用方意外改变包位置和长度。
bool Boot_PackageVerifyFlash(const Boot_PackageFlashSource *source,
                             const uint8_t *public_key, const uint8_t *aes_key,
                             uint32_t *firmware_size)
{
  if (firmware_size == NULL)
  {
    return false;
  }
  *firmware_size = 0U;
  if (source == NULL)
  {
    return false;
  }
  Boot_PackageFlashSource snapshot = *source;
  return Boot_PackageVerify(Boot_PackageReadFlash, &snapshot, snapshot.size,
                             public_key, aes_key, firmware_size);
}
