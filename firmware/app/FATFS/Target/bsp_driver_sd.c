/* USER CODE BEGIN Header */
/**
 ******************************************************************************
  * @file    bsp_driver_sd.c for F4 (based on stm324x9i_eval_sd.c)
 * @brief   This file includes a generic uSD card driver.
 *          To be completed by the user according to the board used for the project.
 * @note    Some functions generated as weak: they can be overridden by
 *          - code in user files
 *          - or BSP code from the FW pack files
 *          if such files are added to the generated project (by the user).
 ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
 ******************************************************************************
 */
/* USER CODE END Header */

#ifdef OLD_API
/* kept to avoid issue when migrating old projects. */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
#else
/* USER CODE BEGIN FirstSection */
/* can be used to modify / undefine following code or add new definitions */
#include "main.h"   /* 需要 main.h 里的 SD_CARD_DET_GPIO_Port / SD_CARD_DET_Pin */
#include "sdio.h"   /* MX_SDIO_SD_Init 声明 */
/* USER CODE END FirstSection */
/* Includes ------------------------------------------------------------------*/
#include "bsp_driver_sd.h"

/* Extern variables ---------------------------------------------------------*/

extern SD_HandleTypeDef hsd;

/* USER CODE BEGIN BeforeInitSection */
/* can be used to modify / undefine following code or add code */
/* USER CODE END BeforeInitSection */
/**
  * @brief  Initializes the SD card device.
  * @retval SD status
  */
__weak uint8_t BSP_SD_Init(void)
{
  uint8_t sd_state = MSD_OK;
  /* Check if the SD card is plugged in the slot */
  if (BSP_SD_IsDetected() != SD_PRESENT)
  {
    return MSD_ERROR;
  }

  /*
   * ================================================================
   * 热插拔后 SDIO 子系统可能处于脏状态：
   *   1. SDIO 核心寄存器 (CR1, CLKCR, FIFO 等) 可能在拔出瞬间被污染
   *   2. DMA2_Stream3 (SDIO RX DMA) 可能还在 active 跑
   *   3. 物理卡没断电但被之前的命令序列搞乱内部状态机
   *
   * 之前的方案（POWER=0, ICR/MASK 清, hsd.State=RESET）都不够——
   * SDIO_Init 虽然重配了核心寄存器，但 DMA 可能还在捣乱。
   *
   * 最彻底的方案：
   *   a) 停 DMA stream
   *   b) RCC reset SDIO + DMA2 外设（所有寄存器回到 reset 默认值）
   *   c) 重跑 MX_SDIO_SD_Init（CubeMX 生成的完整初始化，和 boot 路径一致）
   *   d) MX_SDIO_SD_Init 里的 USER CODE BEGIN 2 已经调了 HAL_SD_Init
   *      → HAL_SD_MspInit (GPIO/DMA/IRQ 幂等重配)
   *      → HAL_SD_InitCard (SD_PowerON: CMD0+CMD8+ACMD41)
   * ================================================================
   */
  __HAL_RCC_SDIO_CLK_ENABLE();
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* a) 先停 DMA2_Stream3 (防止 DMA 在 reset 过程中访问 SDIO FIFO) */
  (void)HAL_DMA_Abort(hsd.hdmarx);

  /* b) RCC reset SDIO (APB2) + DMA2 (AHB1) — 所有寄存器回到 boot 默认值 */
  __HAL_RCC_SDIO_FORCE_RESET();
  __HAL_RCC_DMA2_FORCE_RESET();
  __NOP(); __NOP(); __NOP(); __NOP();
  __HAL_RCC_SDIO_RELEASE_RESET();
  __HAL_RCC_DMA2_RELEASE_RESET();
  __NOP(); __NOP(); __NOP(); __NOP();

  /* 给卡一个稳定窗口（拔插后卡内部需要时间 reset） */
  HAL_Delay(30);

  /* 强制 HAL 句柄回到 RESET — 让 HAL_SD_Init 里能重新调 HAL_SD_MspInit
   * （刚才 RCC reset DMA2 了，必须让 HAL 重新配置 DMA stream）
   */
  hsd.State = HAL_SD_STATE_RESET;
  hsd.ErrorCode = HAL_SD_ERROR_NONE;

  /* c) 完整重跑 MX_SDIO_SD_Init — 和 boot 时路径完全一致
   *    MX_SDIO_SD_Init 内部 USER CODE BEGIN 2 调了 HAL_SD_Init
   *    → HAL_SD_MspInit (GPIO + DMA2_Stream3 + IRQ 全部重配)
   *    → HAL_SD_InitCard (SDIO_Init + SD_PowerON: CMD0+CMD8+ACMD41)
   *
   *    注意：MX_SDIO_SD_Init 只配 PC8-PC12 (SDIO) + PD2 (CMD)
   *    不会碰 PD3 (SD 卡检测 EXTI)，所以 EXTI 配置安全
   */
  MX_SDIO_SD_Init();

  /* 检查 HAL_SD_InitCard 结果 */
  if (hsd.State == HAL_SD_STATE_READY || hsd.State == HAL_SD_STATE_TRANSFER)
  {
    sd_state = MSD_OK;
  }
  else
  {
    sd_state = MSD_ERROR;
  }

  /* 真实数据通路验证：直接轮询读扇区 0。
   * 热插拔后 HAL_SD_GetCardState() 常返回非 TRANSFER，
   * 但卡本身可能是好的 —— 只有真实读一次才能确定。
   * 读失败就直接返回错误，让 FatFs 立刻拿到 STA_NOINIT，
   * 避免它用坏句柄去读并在 SD_TIMEOUT(30s) 上死等。
   */
  {
    static uint8_t probe_buf[512];
    HAL_StatusTypeDef st = HAL_SD_ReadBlocks(&hsd, probe_buf, 0U, 1U, 1000U);

    if (st != HAL_OK)
    {
      return MSD_ERROR;
    }
    sd_state = MSD_OK;
  }

  return sd_state;
}
/* USER CODE BEGIN AfterInitSection */
/* can be used to modify previous code / undefine following code / add code */
/* USER CODE END AfterInitSection */

/* USER CODE BEGIN InterruptMode */
/**
  * @brief  Configures Interrupt mode for SD detection pin.
  * @retval Returns 0
  */
__weak uint8_t BSP_SD_ITConfig(void)
{
  /* Code to be updated by the user or replaced by one from the FW pack (in a stmxxxx_sd.c file) */

  return (uint8_t)0;
}

/** @brief  SD detect IT treatment
  */
__weak void BSP_SD_DetectIT(void)
{
  /* Code to be updated by the user or replaced by one from the FW pack (in a stmxxxx_sd.c file) */
}
/* USER CODE END InterruptMode */

/* USER CODE BEGIN BeforeReadBlocksSection */
/* can be used to modify previous code / undefine following code / add code */
/* USER CODE END BeforeReadBlocksSection */
/**
  * @brief  Reads block(s) from a specified address in an SD card, in polling mode.
  * @param  pData: Pointer to the buffer that will contain the data to transmit
  * @param  ReadAddr: Address from where data is to be read
  * @param  NumOfBlocks: Number of SD blocks to read
  * @param  Timeout: Timeout for read operation
  * @retval SD status
  */
__weak uint8_t BSP_SD_ReadBlocks(uint32_t *pData, uint32_t ReadAddr, uint32_t NumOfBlocks, uint32_t Timeout)
{
  uint8_t sd_state = MSD_OK;

  if (HAL_SD_ReadBlocks(&hsd, (uint8_t *)pData, ReadAddr, NumOfBlocks, Timeout) != HAL_OK)
  {
    sd_state = MSD_ERROR;
  }

  return sd_state;
}

/* USER CODE BEGIN BeforeWriteBlocksSection */
/* can be used to modify previous code / undefine following code / add code */
/* USER CODE END BeforeWriteBlocksSection */
/**
  * @brief  Writes block(s) to a specified address in an SD card, in polling mode.
  * @param  pData: Pointer to the buffer that will contain the data to transmit
  * @param  WriteAddr: Address from where data is to be written
  * @param  NumOfBlocks: Number of SD blocks to write
  * @param  Timeout: Timeout for write operation
  * @retval SD status
  */
__weak uint8_t BSP_SD_WriteBlocks(uint32_t *pData, uint32_t WriteAddr, uint32_t NumOfBlocks, uint32_t Timeout)
{
  uint8_t sd_state = MSD_OK;

  if (HAL_SD_WriteBlocks(&hsd, (uint8_t *)pData, WriteAddr, NumOfBlocks, Timeout) != HAL_OK)
  {
    sd_state = MSD_ERROR;
  }

  return sd_state;
}

/* USER CODE BEGIN BeforeReadDMABlocksSection */
/* can be used to modify previous code / undefine following code / add code */
/* USER CODE END BeforeReadDMABlocksSection */
/**
  * @brief  Reads block(s) from a specified address in an SD card, in DMA mode.
  * @param  pData: Pointer to the buffer that will contain the data to transmit
  * @param  ReadAddr: Address from where data is to be read
  * @param  NumOfBlocks: Number of SD blocks to read
  * @retval SD status
  */
__weak uint8_t BSP_SD_ReadBlocks_DMA(uint32_t *pData, uint32_t ReadAddr, uint32_t NumOfBlocks)
{
  uint8_t sd_state = MSD_OK;

  /* Read block(s) in DMA transfer mode */
  if (HAL_SD_ReadBlocks_DMA(&hsd, (uint8_t *)pData, ReadAddr, NumOfBlocks) != HAL_OK)
  {
    sd_state = MSD_ERROR;
  }

  return sd_state;
}

/* USER CODE BEGIN BeforeWriteDMABlocksSection */
/* can be used to modify previous code / undefine following code / add code */
/* USER CODE END BeforeWriteDMABlocksSection */
/**
  * @brief  Writes block(s) to a specified address in an SD card, in DMA mode.
  * @param  pData: Pointer to the buffer that will contain the data to transmit
  * @param  WriteAddr: Address from where data is to be written
  * @param  NumOfBlocks: Number of SD blocks to write
  * @retval SD status
  */
__weak uint8_t BSP_SD_WriteBlocks_DMA(uint32_t *pData, uint32_t WriteAddr, uint32_t NumOfBlocks)
{
  uint8_t sd_state = MSD_OK;

  /* Write block(s) in DMA transfer mode */
  if (HAL_SD_WriteBlocks_DMA(&hsd, (uint8_t *)pData, WriteAddr, NumOfBlocks) != HAL_OK)
  {
    sd_state = MSD_ERROR;
  }

  return sd_state;
}

/* USER CODE BEGIN BeforeEraseSection */
/* can be used to modify previous code / undefine following code / add code */
/* USER CODE END BeforeEraseSection */
/**
  * @brief  Erases the specified memory area of the given SD card.
  * @param  StartAddr: Start byte address
  * @param  EndAddr: End byte address
  * @retval SD status
  */
__weak uint8_t BSP_SD_Erase(uint32_t StartAddr, uint32_t EndAddr)
{
  uint8_t sd_state = MSD_OK;

  if (HAL_SD_Erase(&hsd, StartAddr, EndAddr) != HAL_OK)
  {
    sd_state = MSD_ERROR;
  }

  return sd_state;
}

/**
  * @brief  Gets the current SD card data status.
  * @param  None
  * @retval Data transfer state.
  *          This value can be one of the following values:
  *            @arg  SD_TRANSFER_OK: No data transfer is acting
  *            @arg  SD_TRANSFER_BUSY: Data transfer is acting
  */
__weak uint8_t BSP_SD_GetCardState(void)
{
  return ((HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_TRANSFER ) ? SD_TRANSFER_OK : SD_TRANSFER_BUSY);
}

/**
  * @brief  Get SD information about specific SD card.
  * @param  CardInfo: Pointer to HAL_SD_CardInfoTypedef structure
  * @retval None
  */
__weak void BSP_SD_GetCardInfo(HAL_SD_CardInfoTypeDef *CardInfo)
{
  /* Get SD card Information */
  HAL_SD_GetCardInfo(&hsd, CardInfo);
}

/* USER CODE BEGIN BeforeCallBacksSection */
/* can be used to modify previous code / undefine following code / add code */
/* USER CODE END BeforeCallBacksSection */
/**
  * @brief SD Abort callbacks
  * @param hsd: SD handle
  * @retval None
  */
void HAL_SD_AbortCallback(SD_HandleTypeDef *hsd)
{
  BSP_SD_AbortCallback();
}

/**
  * @brief Tx Transfer completed callback
  * @param hsd: SD handle
  * @retval None
  */
void HAL_SD_TxCpltCallback(SD_HandleTypeDef *hsd)
{
  BSP_SD_WriteCpltCallback();
}

/**
  * @brief Rx Transfer completed callback
  * @param hsd: SD handle
  * @retval None
  */
void HAL_SD_RxCpltCallback(SD_HandleTypeDef *hsd)
{
  BSP_SD_ReadCpltCallback();
}

/* USER CODE BEGIN CallBacksSection_C */
/**
  * @brief BSP SD Abort callback
  * @retval None
  * @note empty (up to the user to fill it in or to remove it if useless)
  */
__weak void BSP_SD_AbortCallback(void)
{

}

/**
  * @brief BSP Tx Transfer completed callback
  * @retval None
  * @note empty (up to the user to fill it in or to remove it if useless)
  */
__weak void BSP_SD_WriteCpltCallback(void)
{

}

/**
  * @brief BSP Rx Transfer completed callback
  * @retval None
  * @note empty (up to the user to fill it in or to remove it if useless)
  */
__weak void BSP_SD_ReadCpltCallback(void)
{

}
/* USER CODE END CallBacksSection_C */
#endif

/**
 * @brief  Detects if SD card is correctly plugged in the memory slot or not.
 * @param  None
 * @retval Returns if SD is detected or not
 */
__weak uint8_t BSP_SD_IsDetected(void)
{
  __IO uint8_t status = SD_PRESENT;

  /* USER CODE BEGIN 1 */
  /* PD3 卡座检测（硬件：卡插入时机械开关拉低 DET 引脚） */
  return (HAL_GPIO_ReadPin(SD_CARD_DET_GPIO_Port, SD_CARD_DET_Pin) == GPIO_PIN_RESET)
         ? SD_PRESENT : SD_NOT_PRESENT;
  /* USER CODE END 1 */

  return status;
}

/* USER CODE BEGIN AdditionalCode */
/* user code can be inserted here */
/* USER CODE END AdditionalCode */
