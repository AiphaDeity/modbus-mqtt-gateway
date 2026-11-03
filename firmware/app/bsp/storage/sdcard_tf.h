#ifndef SDCARD_TF_H
#define SDCARD_TF_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * TF 卡（SD 卡）SDIO 驱动。
 * 硬件：SDIO 接口 + 4位数据线模式。
 * 依赖：CubeMX 已生成 MX_SDIO_Init()，全局句柄 hsd 可用。
 *
 * 本驱动只提供裸扇区读写（512B/块）。
 * 上层若要使用 FAT 文件系统，需自行集成 FatFs 中间件。
 */

#define SDCARD_BLOCK_SIZE 512U

// 初始化：检测 TF 卡、读取 CSD/CID、初始化 SDIO
HAL_StatusTypeDef sdcard_tf_init(void);

// 判断 TF 卡是否已初始化并可用
bool sdcard_tf_is_ready(void);

// 读取连续扇区
// start_block: 起始块号（每块 512B）
// block_count: 要读的块数
HAL_StatusTypeDef sdcard_tf_read(uint32_t start_block, uint8_t *buf, uint32_t block_count);

// 写入连续扇区
HAL_StatusTypeDef sdcard_tf_write(uint32_t start_block, const uint8_t *buf, uint32_t block_count);

// 读取 TF 卡总容量（字节）
uint64_t sdcard_tf_get_capacity(void);

#endif /* SDCARD_TF_H */
