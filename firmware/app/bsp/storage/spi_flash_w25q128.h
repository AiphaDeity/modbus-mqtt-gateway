#ifndef SPI_FLASH_W25Q128_H
#define SPI_FLASH_W25Q128_H

#include "stm32f4xx_hal.h"
#include "cmsis_os2.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * W25Q128 SPI Flash 驱动。
 * 硬件：SPI2，128Mbit = 16MB，4096字节扇区，64KB块。
 * 依赖：CubeMX 已生成 MX_SPI2_Init()，全局句柄 hspi2 可用。
 *       需确认 CS 片选引脚（CS 必须由软件手动拉低/拉高）。
 *
 * 擦除单位为扇区（4KB），写入单位为页（256B）。
 * 擦除会将整个扇区全部置 0xFF，写入前必须先擦除目标区域。
 */

#define W25Q128_FLASH_SIZE_BYTES  (16UL * 1024UL * 1024UL)
#define W25Q128_SECTOR_SIZE_BYTES (4UL * 1024UL)
#define W25Q128_PAGE_SIZE_BYTES   (256U)

/*
 * SPI2 总线互斥锁（W25Q128 独占 SPI2）。
 * 由 spi_flash_w25q128_init() 创建；下方所有 Flash API 内部已自动加锁，
 * 因此外部（services 层、其他任务）直接调用即可，无需自己加锁。
 * 锁为递归锁，允许在同一线程内嵌套调用（如 write → wait_ready）。
 */
extern osMutexId_t g_m3_spi2_mutex;

// 初始化：读 JEDEC ID，确认芯片在线
HAL_StatusTypeDef spi_flash_w25q128_init(void);

// 读 JEDEC ID（3字节，厂家ID + 容量ID），返回 0xFFFFFF 表示失败
uint32_t spi_flash_w25q128_read_id(void);

// 读数据
HAL_StatusTypeDef spi_flash_w25q128_read(uint32_t addr, uint8_t *buf, uint32_t len);

// 擦除一个扇区（4KB）
HAL_StatusTypeDef spi_flash_w25q128_sector_erase(uint32_t addr);

// 写数据（自动跨页；目标区域必须先擦除）
HAL_StatusTypeDef spi_flash_w25q128_write(uint32_t addr, const uint8_t *buf, uint32_t len);

// 等待写入完成（轮询忙位，有超时保护）
HAL_StatusTypeDef spi_flash_w25q128_wait_ready(uint32_t timeout_ms);

#endif /* SPI_FLASH_W25Q128_H */
