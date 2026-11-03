#ifndef FLASH_PARTITION_H
#define FLASH_PARTITION_H

#include <stdint.h>

/*
 * W25Q128 (16MiB) 外部 SPI Flash 分区定义。
 * 依据系统设计说明书第 7.5 节。
 * 地址为相对偏移（半开区间 [start, end)）。
 */

/* === 配置区：A/B/C 三槽，每槽 80KiB === */
#define FLASH_PART_CONFIG_BASE       0x000000U
#define FLASH_PART_CONFIG_END        0x040000U   /* 256KiB */
#define FLASH_CONFIG_SLOT_SIZE       0x014000U   /* 80KiB 每槽 */
#define FLASH_CONFIG_SLOT_A_BASE     0x000000U
#define FLASH_CONFIG_SLOT_B_BASE     0x014000U
#define FLASH_CONFIG_SLOT_C_BASE     0x028000U
#define FLASH_CONFIG_JSON_MAX        0x010000U   /* 64KiB JSON */
#define FLASH_CONFIG_RESERVED        0x004000U   /* 16KiB 保留 */

/* === 身份、序列高水位与元数据 === */
#define FLASH_PART_IDENTITY_BASE     0x040000U
#define FLASH_PART_IDENTITY_END      0x080000U   /* 256KiB */

/* === 控制日志：双区，各 512KiB === */
#define FLASH_PART_CTRLLOG_BASE      0x080000U
#define FLASH_PART_CTRLLOG_END       0x180000U   /* 1MiB */
#define FLASH_CTRLLOG_AREA_SIZE      0x080000U   /* 512KiB 每区 */
#define FLASH_CTRLLOG_AREA0_BASE     0x080000U
#define FLASH_CTRLLOG_AREA1_BASE     0x100000U

/* === 固件包槽 A/B === */
#define FLASH_PART_FW_A_BASE         0x180000U
#define FLASH_PART_FW_A_END          0x280000U   /* 1MiB */
#define FLASH_PART_FW_B_BASE         0x280000U
#define FLASH_PART_FW_B_END          0x380000U   /* 1MiB */

/* === 升级事务日志 === */
#define FLASH_PART_UPGLOG_BASE       0x380000U
#define FLASH_PART_UPGLOG_END        0x3C0000U   /* 256KiB */

/* === 保留区 === */
#define FLASH_PART_RESERVED_BASE     0x3C0000U
#define FLASH_PART_RESERVED_END      0x1000000U  /* 12.25MiB */

#endif /* FLASH_PARTITION_H */
