#include "boot_package_flash.h"
#include "upgrade_baseline.h"

#include "spi.h"
#include "flash_partition.h"
#include "boot_upgrade_state.h"
#include "boot_recovery.h"
#include "boot_watchdog.h"

#include <string.h>

#define BOOT_FLASH_READ_MAX 256U
#define BOOT_FLASH_SECTOR_SIZE 4096U
// 兼容当前支持的最慢 256 分频：256 字节约需 12.5ms，留出软件开销。
#define BOOT_FLASH_DATA_TIMEOUT_MS 50U
#define BOOT_FLASH_ERASE_TIMEOUT_MS 3000U
#define BOOT_FLASH_PROGRAM_TIMEOUT_MS 3000U

// 检查状态寄存器 1，SPI 失败时不使用接收缓冲中的值。
static HAL_StatusTypeDef Boot_FlashReadStatus(uint8_t *value)
{
  uint8_t command = 0x05U;
  *value = 0xFFU; // 全双工Receive会同时发送缓冲内容，状态读取也固定发送FF。
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_RESET);
  HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi3, &command, 1U, 10U);
  if (status == HAL_OK)
  {
    status = HAL_SPI_Receive(&hspi3, value, 1U, 10U);
  }
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_SET);
  return status;
}

// 写使能必须由芯片的 WEL 位确认，不把命令发送成功当成芯片已接受写入。
static HAL_StatusTypeDef Boot_FlashWriteEnable(void)
{
  uint8_t command = 0x06U;
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_RESET);
  HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi3, &command, 1U, 10U);
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_SET);
  if (status != HAL_OK)
  {
    return status;
  }
  uint8_t value = 0U;
  status = Boot_FlashReadStatus(&value);
  if ((status == HAL_OK) && ((value & 0x03U) != 0x02U))
  {
    return HAL_ERROR;
  }
  return status;
}

// 先做纯地址检查，非法区间不访问SPI；返回1(A)/2(B)，0表示非法。
static uint32_t Boot_FlashPackageSlot(uint32_t address, uint32_t size)
{
  if (size == 0U)
  {
    return 0U;
  }
  if ((address >= FLASH_PART_FW_A_BASE) && (address < FLASH_PART_FW_A_END)
      && (size <= FLASH_PART_FW_A_END - address))
  {
    return 1U;
  }
  if ((address >= FLASH_PART_FW_B_BASE) && (address < FLASH_PART_FW_B_END)
      && (size <= FLASH_PART_FW_B_END - address))
  {
    return 2U;
  }
  return 0U;
}

// 检查包区间及普通升级写权限，拒绝恢复屏障或受保护槽。
static bool Boot_FlashWritableRange(uint32_t address, uint32_t size)
{
  uint32_t slot = Boot_FlashPackageSlot(address, size);
  return (slot != 0U) && Boot_RecoveryNormalWritesAllowed() && Boot_UpgradeSlotWritable(slot);
}

// 每次轮询释放 CS；无符号减法允许 HAL 毫秒计数正常回绕。
HAL_StatusTypeDef Boot_FlashWaitReady(uint32_t timeout_ms)
{
  if (timeout_ms > 1000U)
  {
    return HAL_ERROR;
  }
  uint32_t start = HAL_GetTick();
  do
  {
    uint8_t value = 0U;
    HAL_StatusTypeDef status = Boot_FlashReadStatus(&value);
    if (status != HAL_OK)
    {
      return status;
    }
    if ((value & 0x01U) == 0U)
    {
      return HAL_OK;
    }
  } while ((uint32_t)(HAL_GetTick() - start) < timeout_ms);
  return HAL_TIMEOUT;
}

// 编程和擦除共用命令/地址阶段，发送失败也立即释放片选。
static HAL_StatusTypeDef Boot_FlashModify(uint8_t instruction, uint32_t address,
                                         const uint8_t *data, uint16_t size,
                                         uint32_t timeout_ms)
{
  // 角色检查可能读取日志，但锁存失败后不得再发送任何擦写命令。
  if (Boot_WatchdogFailed())
  {
    return HAL_ERROR;
  }
  HAL_StatusTypeDef status = Boot_FlashWaitReady(1000U);
  if (status != HAL_OK)
  {
    return status;
  }
  status = Boot_FlashWriteEnable();
  if (status != HAL_OK)
  {
    return status;
  }
  uint8_t command[4] =
  {
    instruction, (uint8_t)(address >> 16U), (uint8_t)(address >> 8U), (uint8_t)address
  };
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_RESET);
  status = HAL_SPI_Transmit(&hspi3, command, sizeof(command), 10U);
  if ((status == HAL_OK) && (size > 0U))
  {
    status = HAL_SPI_Transmit(&hspi3, (uint8_t *)data, size, BOOT_FLASH_DATA_TIMEOUT_MS);
  }
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_SET);
  if (status != HAL_OK)
  {
    return status;
  }
  return Boot_FlashWaitReady(timeout_ms);
}

// 必须读回整个扇区，保护状态导致擦除被忽略时不能报告成功。
static HAL_StatusTypeDef Boot_FlashEraseChecked(uint32_t address)
{
  uint32_t started_ms = HAL_GetTick();
  if (!Boot_WatchdogCheck(started_ms, BOOT_FLASH_ERASE_TIMEOUT_MS))
  {
    return HAL_ERROR;
  }
  HAL_StatusTypeDef status = Boot_FlashModify(0x20U, address, NULL, 0U, 1000U);
  if (!Boot_WatchdogCheck(started_ms, BOOT_FLASH_ERASE_TIMEOUT_MS))
  {
    return HAL_TIMEOUT;
  }
  if (status != HAL_OK)
  {
    return status;
  }
  uint8_t buffer[BOOT_FLASH_READ_MAX];
  for (uint32_t offset = 0U; offset < BOOT_FLASH_SECTOR_SIZE; offset += sizeof(buffer))
  {
    if (!Boot_WatchdogCheck(started_ms, BOOT_FLASH_ERASE_TIMEOUT_MS))
    {
      return HAL_TIMEOUT;
    }
    status = Boot_FlashRead(address + offset, buffer, sizeof(buffer));
    if (!Boot_WatchdogCheck(started_ms, BOOT_FLASH_ERASE_TIMEOUT_MS))
    {
      return HAL_TIMEOUT;
    }
    if (status != HAL_OK)
    {
      return status;
    }
    for (size_t i = 0U; i < sizeof(buffer); i++)
    {
      if (buffer[i] != 0xFFU)
      {
        return HAL_ERROR;
      }
    }
  }
  // 整个扇区擦除且读回全FF后才报告进展，忙轮询和读回中途均不喂狗。
  return Boot_WatchdogProgress(started_ms, BOOT_FLASH_ERASE_TIMEOUT_MS)
       ? HAL_OK : HAL_TIMEOUT;
}

// 检查数据非空、长度及页边界，不判断目标区域权限。
static bool Boot_FlashPageValid(uint32_t address, const uint8_t *data, uint16_t size)
{
  return (data != NULL) && (size != 0U) && (size <= BOOT_FLASH_READ_MAX)
      && (size <= BOOT_FLASH_READ_MAX - (address % BOOT_FLASH_READ_MAX));
}

// 不自动跨页、不隐式擦除，写完读回比对以发现写保护或编程失败。
static HAL_StatusTypeDef Boot_FlashProgramChecked(uint32_t address, const uint8_t *data, uint16_t size)
{
  if (!Boot_FlashPageValid(address, data, size))
  {
    return HAL_ERROR;
  }
  uint32_t started_ms = HAL_GetTick();
  if (!Boot_WatchdogCheck(started_ms, BOOT_FLASH_PROGRAM_TIMEOUT_MS))
  {
    return HAL_ERROR;
  }
  HAL_StatusTypeDef status = Boot_FlashModify(0x02U, address, data, size, 20U);
  if (!Boot_WatchdogCheck(started_ms, BOOT_FLASH_PROGRAM_TIMEOUT_MS))
  {
    return HAL_TIMEOUT;
  }
  if (status != HAL_OK)
  {
    return status;
  }
  uint8_t buffer[BOOT_FLASH_READ_MAX];
  status = Boot_FlashRead(address, buffer, size);
  if (!Boot_WatchdogCheck(started_ms, BOOT_FLASH_PROGRAM_TIMEOUT_MS))
  {
    return HAL_TIMEOUT;
  }
  if ((status == HAL_OK) && (memcmp(data, buffer, size) != 0))
  {
    return HAL_ERROR;
  }
  // 页已实际编程并读回一致，才允许刷新；失败的SPI操作不算进展。
  if (status != HAL_OK)
  {
    return status;
  }
  return Boot_WatchdogProgress(started_ms, BOOT_FLASH_PROGRAM_TIMEOUT_MS)
       ? HAL_OK : HAL_TIMEOUT;
}

// 按普通包槽保护规则擦除并读回核对一个扇区。
HAL_StatusTypeDef Boot_FlashEraseSector(uint32_t address)
{
  if (((address % BOOT_FLASH_SECTOR_SIZE) != 0U)
      || !Boot_FlashWritableRange(address, BOOT_FLASH_SECTOR_SIZE))
  {
    return HAL_ERROR;
  }
  return Boot_FlashEraseChecked(address);
}

// 按普通包槽保护规则编程并读回核对一页数据。
HAL_StatusTypeDef Boot_FlashProgramPage(uint32_t address, const uint8_t *data, uint16_t size)
{
  if (!Boot_FlashPageValid(address, data, size) || !Boot_FlashWritableRange(address, size))
  {
    return HAL_ERROR;
  }
  return Boot_FlashProgramChecked(address, data, size);
}

// 在普通升级日志区擦除一个扇区。
HAL_StatusTypeDef Boot_FlashJournalErase(uint32_t address)
{
  if ((address != FLASH_PART_UPGLOG_BASE)
      && (address != FLASH_PART_UPGLOG_BASE + BOOT_FLASH_SECTOR_SIZE))
  {
    return HAL_ERROR;
  }
  return Boot_FlashEraseChecked(address);
}

// 在普通升级日志区编程一页内的数据。
HAL_StatusTypeDef Boot_FlashJournalProgram(uint32_t address, const uint8_t *data, uint16_t size)
{
  if ((address < FLASH_PART_UPGLOG_BASE)
      || (address >= FLASH_PART_UPGLOG_BASE + 2U * BOOT_FLASH_SECTOR_SIZE)
      || (size > FLASH_PART_UPGLOG_BASE + 2U * BOOT_FLASH_SECTOR_SIZE - address))
  {
    return HAL_ERROR;
  }
  return Boot_FlashProgramChecked(address, data, size);
}

// 仅在当前恢复事务允许的包槽擦除扇区。
HAL_StatusTypeDef Boot_FlashRecoveryErase(uint32_t address)
{
  if (((address % BOOT_FLASH_SECTOR_SIZE) != 0U)
      || (Boot_FlashPackageSlot(address, BOOT_FLASH_SECTOR_SIZE) == 0U)
      || !Boot_RecoveryWriteAllowed(address, BOOT_FLASH_SECTOR_SIZE))
  {
    return HAL_ERROR;
  }
  return Boot_FlashEraseChecked(address);
}

// 仅在当前恢复事务允许的包槽编程数据。
HAL_StatusTypeDef Boot_FlashRecoveryProgram(uint32_t address, const uint8_t *data, uint16_t size)
{
  if (!Boot_FlashPageValid(address, data, size) || (Boot_FlashPackageSlot(address, size) == 0U)
      || !Boot_RecoveryWriteAllowed(address, size))
  {
    return HAL_ERROR;
  }
  return Boot_FlashProgramChecked(address, data, size);
}

// 在独立恢复日志或屏障区擦除扇区。
HAL_StatusTypeDef Boot_FlashRecoveryJournalErase(uint32_t address)
{
  uint32_t start = FLASH_PART_UPGLOG_BASE + BOOT_RECOVERY_LOG_OFFSET;
  uint32_t end = FLASH_PART_UPGLOG_BASE + BOOT_RECOVERY_BARRIER_OFFSET + BOOT_FLASH_SECTOR_SIZE;
  if ((address < start) || (address >= end) || ((address % BOOT_FLASH_SECTOR_SIZE) != 0U))
  {
    return HAL_ERROR;
  }
  return Boot_FlashEraseChecked(address);
}

// 在独立恢复日志或屏障区编程数据。
HAL_StatusTypeDef Boot_FlashRecoveryJournalProgram(uint32_t address, const uint8_t *data, uint16_t size)
{
  uint32_t start = FLASH_PART_UPGLOG_BASE + BOOT_RECOVERY_LOG_OFFSET;
  uint32_t end = FLASH_PART_UPGLOG_BASE + BOOT_RECOVERY_BARRIER_OFFSET + BOOT_FLASH_SECTOR_SIZE;
  if ((address < start) || (address >= end) || (size > end - address))
  {
    return HAL_ERROR;
  }
  return Boot_FlashProgramChecked(address, data, size);
}

// 按芯片偏移同步读取指定字节，数据阶段发送固定的0xFF占位字节。
HAL_StatusTypeDef Boot_FlashRead(uint32_t address, uint8_t *data, uint16_t size)
{
  // 检查缓冲、长度和地址范围
  if ((data == NULL) || (size == 0U) || (size > BOOT_FLASH_READ_MAX)
      || (address >= FLASH_CAPACITY_BYTES) || (size > FLASH_CAPACITY_BYTES - address))
  {
    return HAL_ERROR;
  }
  uint8_t command[4] =
  {
    0x03U, (uint8_t)(address >> 16U), (uint8_t)(address >> 8U), (uint8_t)address
  };
  // 双线主机的HAL接收会同时发送data原内容，先填固定值再开始事务。
  memset(data, 0xFF, size);
  // CS 拉低，发送 0x03 + 三字节地址，发送成功后接收数据，CS 拉高
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_RESET);
  HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi3, command, sizeof(command), 10U);
  if (status == HAL_OK)
  {
    status = HAL_SPI_Receive(&hspi3, data, size, BOOT_FLASH_DATA_TIMEOUT_MS);
  }
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_SET);
  return status;
}

// 用减法先检查区间，再计算物理偏移，避免不可信长度触发加法溢出。
HAL_StatusTypeDef Boot_FlashTransactionErase(uint32_t address)
{
  uint32_t start = FLASH_PART_UPGLOG_BASE + UPGRADE_STATE_TRANSACTION_OFFSET;
  if ((address == start) || (address == start + BOOT_FLASH_SECTOR_SIZE))
  {
    return Boot_FlashEraseChecked(address);
  }
  return HAL_ERROR;
}

// 仅在事务号双副本区编程数据。
HAL_StatusTypeDef Boot_FlashTransactionProgram(uint32_t address, const uint8_t *data, uint16_t size)
{
  uint32_t start = FLASH_PART_UPGLOG_BASE + UPGRADE_STATE_TRANSACTION_OFFSET;
  uint32_t end = start + 2U * BOOT_FLASH_SECTOR_SIZE;
  if ((address >= start) && (address < end) && (size <= end - address))
  {
    return Boot_FlashProgramChecked(address, data, size);
  }
  return HAL_ERROR;
}

// 检查包内偏移及长度，将读取请求映射到外部 Flash。
bool Boot_PackageReadFlash(void *context, size_t offset, uint8_t *data, size_t size)
{
  const Boot_PackageFlashSource *source = context;
  if ((source == NULL) || (data == NULL) || (size == 0U)
      || (size > BOOT_FLASH_READ_MAX)
      || (source->address >= FLASH_CAPACITY_BYTES)
      || (source->size > FLASH_CAPACITY_BYTES - source->address)
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

// 普通上下文同步读；仅允许访问共用元数据范围，不开放任意物理地址。
bool Boot_FlashReadMetadata(void *context, uint32_t offset, uint8_t *data, uint32_t size)
{
  (void)context;
  if ((data == NULL) || (size == 0U) || (size > 256U)
      || (offset >= UPGRADE_STATE_METADATA_SIZE)
      || (size > UPGRADE_STATE_METADATA_SIZE - offset))
  {
    return false;
  }
  return Boot_FlashRead(FLASH_PART_UPGLOG_BASE + offset, data, (uint16_t)size) == HAL_OK;
}
