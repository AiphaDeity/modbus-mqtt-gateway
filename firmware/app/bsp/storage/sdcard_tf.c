/*
 * sdcard_tf.c — TF 卡 SDIO 驱动（裸扇区读写版）
 *
 * 硬件：SDIO 接口，4 位数据线
 * 依赖：CubeMX 已生成 MX_SDIO_SD_Init()，全局句柄 hsd
 *       已配置 DMA2_Stream3（SDIO RX）
 *
 * 说明：本驱动只提供 512B 扇区级别的读写。
 *       如需文件系统，请在 CubeMX 启用 FatFs 后在本驱动之上封装。
 */

#include "sdcard_tf.h"
#include "stm32f4xx_hal_sd.h"

extern SD_HandleTypeDef hsd;

static bool s_sd_ready = false;

HAL_StatusTypeDef sdcard_tf_init(void)
{
    HAL_SD_CardInfoTypeDef info;
    HAL_StatusTypeDef st;

    // SDIO 已由 CubeMX 初始化，这里只需确认卡状态
    if (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER) {
        // 尝试重新初始化卡
        st = HAL_SD_InitCard(&hsd);
        if (st != HAL_OK) {
            s_sd_ready = false;
            return HAL_ERROR;
        }
    }

    // 读取卡信息
    st = HAL_SD_GetCardInfo(&hsd, &info);
    if (st != HAL_OK) {
        s_sd_ready = false;
        return HAL_ERROR;
    }

    // 必须是 512 字节块
    if (info.BlockSize != 512U) {
        s_sd_ready = false;
        return HAL_ERROR;
    }

    s_sd_ready = true;
    return HAL_OK;
}

bool sdcard_tf_is_ready(void)
{
    return s_sd_ready;
}

HAL_StatusTypeDef sdcard_tf_read(uint32_t start_block, uint8_t *buf, uint32_t block_count)
{
    if (!s_sd_ready || buf == NULL || block_count == 0)
        return HAL_ERROR;

    HAL_StatusTypeDef st = HAL_SD_ReadBlocks(&hsd, buf, start_block, block_count, 1000);
    if (st != HAL_OK) return st;

    // 轮询等待传输完成
    uint32_t start = HAL_GetTick();
    while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER) {
        if ((HAL_GetTick() - start) > 2000) return HAL_TIMEOUT;
    }
    return HAL_OK;
}

HAL_StatusTypeDef sdcard_tf_write(uint32_t start_block, const uint8_t *buf, uint32_t block_count)
{
    if (!s_sd_ready || buf == NULL || block_count == 0)
        return HAL_ERROR;

    HAL_StatusTypeDef st = HAL_SD_WriteBlocks(&hsd, (uint8_t *)buf, start_block, block_count, 1000);
    if (st != HAL_OK) return st;

    uint32_t start = HAL_GetTick();
    while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER) {
        if ((HAL_GetTick() - start) > 2000) return HAL_TIMEOUT;
    }
    return HAL_OK;
}

uint64_t sdcard_tf_get_capacity(void)
{
    HAL_SD_CardInfoTypeDef info;
    if (HAL_SD_GetCardInfo(&hsd, &info) != HAL_OK)
        return 0;
    return (uint64_t)info.BlockNbr * (uint64_t)info.BlockSize;
}
