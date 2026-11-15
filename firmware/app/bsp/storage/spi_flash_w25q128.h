#ifndef SPI_FLASH_W25Q128_H
#define SPI_FLASH_W25Q128_H

#include "stm32f4xx_hal.h"
#include "cmsis_os2.h"
#include "flash_partition.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * W25Q64 SPI Flash 驱动，沿用原驱动接口。
 * 硬件：SPI3，8MiB，4096字节扇区，64KiB块。
 * 依赖：CubeMX 已生成 MX_SPI3_Init()，全局句柄 hspi3 可用。
 *       PD11/EXT_SPI_FLASH_CS 为片选，原 PE4 和 IMU PE7 始终保持高。
 *
 * 擦除单位为扇区（4KB），写入单位为页（256B）。
 * 擦除会将整个扇区全部置 0xFF，写入前必须先擦除目标区域。
 */

#define W25Q128_FLASH_SIZE_BYTES  FLASH_CAPACITY_BYTES
#define W25Q128_SECTOR_SIZE_BYTES (4UL * 1024UL)
#define W25Q128_PAGE_SIZE_BYTES   (256U)

/*
 * SPI3 总线互斥锁（外部 Flash 独占 SPI3）。
 * 由 spi_flash_w25q128_init() 创建；下方所有 Flash API 内部已自动加锁，
 * 因此外部（services 层、其他任务）直接调用即可，无需自己加锁。
 * 有界读写将取锁、SPI收发和芯片忙等待计入同一绝对期限。
 */
extern osMutexId_t g_m3_spi3_mutex;

// 初始化并检查W25Q64的JEDEC ID，失败不开放存储。
HAL_StatusTypeDef spi_flash_w25q128_init(void);

// 读 JEDEC ID（3字节，厂家ID + 容量ID），返回 0xFFFFFF 表示失败
uint32_t spi_flash_w25q128_read_id(void);

/* 业务和升级共用的有界I/O：deadline_ms为HAL绝对毫秒期限，覆盖取锁、SPI和内部忙等待。
 * read/write每次1~256B；write允许跨页。erase的block64选择64KiB块或4KiB扇区。
 * 返回前结束SPI访问；超时不表示已发出的Flash编程/擦除命令已撤销。 */
HAL_StatusTypeDef spi_flash_w25q128_read_until(uint32_t addr, uint8_t *buf,
                                             uint32_t len, uint32_t deadline_ms);
HAL_StatusTypeDef spi_flash_w25q128_write_until(uint32_t addr, const uint8_t *buf,
                                              uint32_t len, uint32_t deadline_ms);
HAL_StatusTypeDef spi_flash_w25q128_erase_until(uint32_t addr, bool block64,
                                              uint32_t deadline_ms);

// 读取状态用于故障诊断；失败返回0xFF，不将该值作为成功或空闲的依据。
uint8_t spi_flash_w25q128_read_sr1(void);
uint8_t spi_flash_w25q128_read_sr2(void);

#endif /* SPI_FLASH_W25Q128_H */
