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
 * 调用方独占 SPI2，其他从设备片选须保持高。命令和数据阶段各限时 10ms。
 * 错误返回 HAL 状态且释放 CS，data 可能已被部分改写，失败数据不得使用。
 */
HAL_StatusTypeDef Boot_FlashRead(uint32_t address, uint8_t *data, uint16_t size);

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

#endif
