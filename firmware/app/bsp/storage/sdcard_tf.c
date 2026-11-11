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
#include "main.h"   /* SDIO_D0_Pin, SDIO_CMD_Pin 等 */
#include "stm32f4xx_hal_sd.h"
#include "ff_gen_drv.h"   /* Disk_drvTypeDef / disk.is_initialized */
#include "cmsis_os2.h"
#include "sdio.h"
#include "bsp_driver_sd.h"

extern SD_HandleTypeDef hsd;
extern Disk_drvTypeDef  disk;

static bool s_sd_ready = false;

/* 重建SDIO和本卡DMA流；覆盖生成的弱实现，供首次挂载和热插拔恢复复用。 */
uint8_t BSP_SD_Init(void)
{
    if (BSP_SD_IsDetected() != SD_PRESENT) return MSD_ERROR;

    __HAL_RCC_SDIO_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    if (hsd.hdmarx != NULL) {
        if (HAL_DMA_GetState(hsd.hdmarx) == HAL_DMA_STATE_BUSY &&
            HAL_DMA_Abort(hsd.hdmarx) != HAL_OK) return MSD_ERROR;
        if (HAL_DMA_DeInit(hsd.hdmarx) != HAL_OK) return MSD_ERROR;
    }
    /* 只复位SDIO；DMA2上的其他串口流保持运行。 */
    __HAL_RCC_SDIO_FORCE_RESET();
    __DSB();
    __HAL_RCC_SDIO_RELEASE_RESET();
    HAL_Delay(30U);

    hsd.State = HAL_SD_STATE_RESET;
    hsd.ErrorCode = HAL_SD_ERROR_NONE;
    MX_SDIO_SD_Init();
    if (HAL_SD_Init(&hsd) != HAL_OK) return MSD_ERROR;

    /* 读扇区0验证真实通路；读失败不得报告初始化成功。 */
    static uint8_t probe_buf[SDCARD_BLOCK_SIZE] __attribute__((aligned(4)));
    return HAL_SD_ReadBlocks(&hsd, probe_buf, 0U, 1U, 1000U) == HAL_OK
           ? MSD_OK : MSD_ERROR;
}

/* 初始化介质后检查卡容量；无卡或枚举失败由上层按不可用处理。 */
HAL_StatusTypeDef sdcard_tf_init(void)
{
    HAL_SD_CardInfoTypeDef info;
    HAL_StatusTypeDef st;

    if (BSP_SD_Init() != MSD_OK) {
        s_sd_ready = false;
        return HAL_ERROR;
    }

    /* 验证卡信息 */
    st = HAL_SD_GetCardInfo(&hsd, &info);
    if (st != HAL_OK) {
        s_sd_ready = false;
        return HAL_ERROR;
    }
    if (info.BlockSize != 512U) {
        s_sd_ready = false;
        return HAL_ERROR;
    }

    s_sd_ready = true;
    return HAL_OK;
}

HAL_StatusTypeDef sdcard_tf_deinit(void)
{
    /*
     * 拔卡路径 — 关键！必须在物理拔卡前（或紧随其后）彻底停掉 SDIO 活动。
     *
     * 为什么？拔卡瞬间：
     *   - 如果 FatFs DMA 还在 active → DMA2_Stream3 继续跑 → 可能访问垃圾数据
     *   - 如果 SDIO POWER=ON + CLK 在 toggle → DAT/CMD 悬空引脚产生振铃
     *   - 振铃通过寄生电容耦合进 SDIO 外设 → 核心状态机混乱
     *
     * 这里做的事情（**全部不发命令给卡！**）：
     *   1) Abort + Disable DMA2_Stream3（防止 DMA 在 FIFO 里留垃圾）
     *   2) SDIO POWER=0（停止 SCLK，让 SDIO 所有输出回到 idle）
     *   3) SDIO CLK=0（彻底关时钟分频器）
     *   4) 清所有 SDIO 中断标志 + disable 中断
     *   5) SDIO GPIO 回到 input floating（避免悬空引脚干扰）
     *
     * 插卡后 BSP_SD_Init 重建SDIO和本卡DMA流。
     */

    /* 1) 停 DMA */
    if (hsd.hdmarx != NULL) {
        (void)HAL_DMA_Abort(hsd.hdmarx);
        (void)HAL_DMA_DeInit(hsd.hdmarx);
    }

    /* 2-4) SDIO 彻底关电源 + 清状态 */
    if (hsd.Instance != NULL) {
        hsd.Instance->POWER = 0;           /* 关电源 → SCLK 停止 */
        hsd.Instance->CLKCR = 0;           /* 清时钟分频器 */
        hsd.Instance->ICR = 0xFFFFFFFF;    /* 清所有 pending flags */
        hsd.Instance->MASK = 0;            /* disable 所有中断 */
        hsd.Instance->DCTRL = 0;           /* 清数据控制 */
        hsd.Instance->CMD = 0;             /* 清命令 */
    }

    /* 5) SDIO GPIO 回到 input floating（防拔卡瞬间悬空干扰） */
    {
        GPIO_InitTypeDef g = {0};
        g.Mode = GPIO_MODE_INPUT;
        g.Pull = GPIO_NOPULL;
        g.Speed = GPIO_SPEED_FREQ_LOW;
        g.Pin = SDIO_D0_Pin | SDIO_D1_Pin | SDIO_D2_Pin | SDIO_D3_Pin | SDIO_CK_Pin;
        HAL_GPIO_Init(GPIOC, &g);
        g.Pin = SDIO_CMD_Pin;
        HAL_GPIO_Init(SDIO_CMD_GPIO_Port, &g);
    }

    /* 等所有状态稳定 */
    HAL_Delay(10);

    /* 关键：清除 FatFs 的“已初始化”标志。
     * diskio.c 的 disk_initialize() 里有一层缓存：
     *   if (disk.is_initialized[pdrv] == 0) { ...才真正调 SD_initialize... }
     * 首次初始化成功后该标志永远为 1，于是热插拔后
     * disk_initialize() 直接返回 OK，SD_initialize() 根本不会被调用，
     * FatFs 继续用拔卡前那个失效的 SDIO 句柄读扇区 → 等 30s 超时。
     */
    disk.is_initialized[0] = 0;

    s_sd_ready = false;
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


/* 升级文件读使用同步单块轮询，避免DMA超时后继续引用调用方缓冲。 */
HAL_StatusTypeDef sdcard_tf_read_until(uint32_t block, uint8_t *buf, uint32_t deadline_ms)
{
    int32_t remaining = (int32_t)(deadline_ms - HAL_GetTick());
    if (remaining <= 0) return HAL_TIMEOUT;
    if (buf == NULL || HAL_GPIO_ReadPin(SD_CARD_DET_GPIO_Port, SD_CARD_DET_Pin) != GPIO_PIN_RESET)
        return HAL_ERROR;
    if (hsd.State != HAL_SD_STATE_READY || hsd.Context != SD_CONTEXT_NONE) return HAL_BUSY;
    if (block >= hsd.SdCard.LogBlockNbr) return HAL_ERROR;

    HAL_StatusTypeDef st = HAL_SD_ReadBlocks(&hsd, buf, block, 1U, (uint32_t)remaining);
    if ((int32_t)(deadline_ms - HAL_GetTick()) <= 0) return HAL_TIMEOUT;
    return st;
}

/* 单扇区同步写入；卡内部忙等待也使用原HAL期限。 */
HAL_StatusTypeDef sdcard_tf_write_until(uint32_t block, const uint8_t *buf, uint32_t deadline_ms)
{
    int32_t remaining = (int32_t)(deadline_ms - HAL_GetTick());
    if (remaining <= 0) return HAL_TIMEOUT;
    if (buf == NULL || HAL_GPIO_ReadPin(SD_CARD_DET_GPIO_Port, SD_CARD_DET_Pin) != GPIO_PIN_RESET)
        return HAL_ERROR;
    if (hsd.State != HAL_SD_STATE_READY || hsd.Context != SD_CONTEXT_NONE) return HAL_BUSY;
    if (block >= hsd.SdCard.LogBlockNbr) return HAL_ERROR;
    HAL_StatusTypeDef st = HAL_SD_WriteBlocks(&hsd, (uint8_t *)buf, block, 1U, (uint32_t)remaining);
    if (st != HAL_OK) return st;
    while ((int32_t)(deadline_ms - HAL_GetTick()) > 0) {
        if (HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_TRANSFER) return HAL_OK;
        osDelay(1U);
    }
    return HAL_TIMEOUT;
}
