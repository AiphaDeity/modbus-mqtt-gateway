#ifndef BOOT_PACKAGE_FLASH_H
#define BOOT_PACKAGE_FLASH_H

#include "boot_package.h"
#include "stm32f4xx_hal.h"

/*
 * 外部 Flash 中一份完整包的位置，地址和长度单位均为字节。
 * address 是芯片内偏移，size 是实际包长，不是整个包槽的容量。
 * 调用方依据已核对的包槽和状态提供位置，验证期间禁止修改源数据。
 */
typedef struct
{
  uint32_t address;
  uint32_t size;
} Boot_PackageFlashSource;

/*
 * SPI2 普通上下文同步读取 W25Q128，单次 1～256 字节，调用前完成 GPIO/SPI 初始化。
 * 调用方独占 SPI2，其他从设备片选须保持高。命令限时 10ms，数据限时 50ms。
 * 错误返回 HAL 状态且释放 CS，data 可能已被部分改写，失败数据不得使用。
 */
HAL_StatusTypeDef Boot_FlashRead(uint32_t address, uint8_t *data, uint16_t size);

/*
 * 轮询 BUSY 位，timeout_ms 为 0～1000ms，0 表示仅检查一次。
 * 空闲返回 HAL_OK，忙至超时返回 HAL_TIMEOUT，SPI 错误直接返回。
 * 必须在 HAL 毫秒时基正常前进的普通上下文调用，调用方独占 SPI2。
 * 单次状态读取的命令/接收各限时 10ms，超时边界可额外包含这次传输。
 */
HAL_StatusTypeDef Boot_FlashWaitReady(uint32_t timeout_ms);

/*
 * 擦除固件包槽内一个 4KiB 扇区，address 必须按 4096 字节对齐。
 * 仅允许 flash_partition.h 定义的固件槽 A/B，不支持整片擦除。
 * 持久状态异常或槽被已确认包/候选包引用时拒绝；调用方仍须独占存储。
 * 先等待空闲并确认写使能，再擦除、等待完成和逐块读回检查全为 FF。
 * 失败不重试，返回后扇区可能已部分或完全擦除，应停止上层写包流程。
 * 擦除及整扇区读回共用3000 ms软件上限，成功后报告进展，忙轮询不喂狗。
 */
HAL_StatusTypeDef Boot_FlashEraseSector(uint32_t address);

/*
 * 向固件包槽写入 1～256 字节，不得跨越 256 字节页边界。
 * data 在调用期间可读且有效。调用方负责事先擦除允许改写的目标扇区。
 * 确认写使能后编程、等待完成并读回比对；失败不重试，内容可能部分改变。
 * 擦写接口均须先完成 GPIO/SPI 初始化，保持其他片选为高，独占 SPI2。
 * 页编程和读回共用3000 ms软件上限，成功后报告进展；监督失败后全部写接口拒绝修改。
 */
HAL_StatusTypeDef Boot_FlashProgramPage(uint32_t address, const uint8_t *data, uint16_t size);

/*
 * 状态日志专用：仅允许升级日志区前两个 4KiB 扇区，沿用擦写/读回/超时约定。
 * 由 boot_upgrade_state 独占调用；不接受包槽或其他业务分区地址。
 */
HAL_StatusTypeDef Boot_FlashJournalErase(uint32_t address);
HAL_StatusTypeDef Boot_FlashJournalProgram(uint32_t address, const uint8_t *data, uint16_t size);

/*
 * 独立恢复专用：仅当前RECEIVING事务选定槽可擦写，仍保护可信confirmed槽。
 * 页边界、4KiB对齐、超时及读回规则同普通接口；不支持整片擦除。
 */
HAL_StatusTypeDef Boot_FlashRecoveryErase(uint32_t address);
HAL_StatusTypeDef Boot_FlashRecoveryProgram(uint32_t address, const uint8_t *data, uint16_t size);

// 恢复元数据专用，仅允许升级日志区[BASE+0x2000, BASE+0x5000)三个4KiB扇区。
HAL_StatusTypeDef Boot_FlashRecoveryJournalErase(uint32_t address);
HAL_StatusTypeDef Boot_FlashRecoveryJournalProgram(uint32_t address, const uint8_t *data, uint16_t size);

/*
 * Boot_PackageRead 回调，context 必须指向有效 Boot_PackageFlashSource。
 * 检查包范围和芯片容量，完整读出才返回 true；失败时 data 内容不可用。
 */
bool Boot_PackageReadFlash(void *context, size_t offset, uint8_t *data, size_t size);

/*
 * 验证外部 Flash 指定位置的整包，公钥/AES 密钥与 Boot_PackageVerify 约定相同。
 * source 在调用期间有效且不变。失败时 firmware_size 置零。
 * 只读取和验证，不擦写存储、不安装、不改变启动或升级状态。
 */
bool Boot_PackageVerifyFlash(const Boot_PackageFlashSource *source,
                             const uint8_t *public_key, const uint8_t *aes_key,
                             uint32_t *firmware_size);

// 仅擦写持久基线 2 的事务号双副本区，不允许访问普通/恢复日志及包槽。
HAL_StatusTypeDef Boot_FlashTransactionErase(uint32_t address);
HAL_StatusTypeDef Boot_FlashTransactionProgram(uint32_t address, const uint8_t *data, uint16_t size);

// Boot日志共用只读回调，offset相对UPGLOG，size为1～256 B；不开放写权限。
bool Boot_FlashReadMetadata(void *context, uint32_t offset, uint8_t *data, uint32_t size);

#endif
