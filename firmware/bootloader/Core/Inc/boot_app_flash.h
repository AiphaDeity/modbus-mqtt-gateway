#ifndef BOOT_APP_FLASH_H
#define BOOT_APP_FLASH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 擦除一个App扇区（4～7），成功返回true，不在HAL忙等待中喂狗。
 * 返回后检查5000 ms软件上限；同Bank擦除可能停HAL时基，仍需IWDG及实板计时。
 */
bool Boot_AppFlashEraseSector(uint32_t sector);

/*
 * 将 RAM 中的 data 写入已擦除的 App 区。address 和 len 须为 4 字节对齐，
 * len 的单位为字节。失败返回 false，目标区域可能已部分写入。
 * 单次编程固定10000 ms软件上限，不逐字喂狗；调用方读回确认后报告进展。
 * 本模块擦写均在Boot普通上下文调用，监督失败锁存后拒绝后续擦写。
 */
bool Boot_AppFlashProgram(uint32_t address, const uint8_t *data, uint32_t len);

// 逐字节比较 App 区和 data，全部一致返回 true。
bool Boot_AppFlashVerify(uint32_t address, const uint8_t *data, uint32_t len);

// 普通上下文同步读取 App 区，len 为1～256字节；data 在调用期间可写，不保留指针。
bool Boot_AppFlashRead(uint32_t address, uint8_t *data, uint32_t len);

#endif
