#ifndef BOOT_APP_FLASH_H
#define BOOT_APP_FLASH_H

#include <stdbool.h>
#include <stdint.h>

// 擦除一个 App 扇区（4～7），成功返回 true。
bool Boot_AppFlashEraseSector(uint32_t sector);

/*
 * 将 RAM 中的 data 写入已擦除的 App 区。address 和 len 须为 4 字节对齐，
 * len 的单位为字节。失败返回 false，目标区域可能已部分写入。
 */
bool Boot_AppFlashProgram(uint32_t address, const uint8_t *data, uint32_t len);

// 逐字节比较 App 区和 data，全部一致返回 true。
bool Boot_AppFlashVerify(uint32_t address, const uint8_t *data, uint32_t len);

#endif
