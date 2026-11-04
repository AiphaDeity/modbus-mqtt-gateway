#include "bsp_rs485.h"
#include "modbus_app.h"
#include <stdint.h>

static UART_HandleTypeDef *s_huart = NULL;  // 内部保存句柄，不再硬绑huart1
static Rs485RxCpltCallback g_rx_cplt_cb = NULL;
static Rs485TxCpltCallback g_tx_cplt_cb = NULL;

extern mb_handle_t *mb_handle;

// 初始化，由上层传入串口句柄
Rs485Status_e BSP_RS485_Init(UART_HandleTypeDef *huart)
{
    if(huart == NULL)
    {
        return RS485_ERR_BUSY;
    }
    s_huart = huart;
    // 在这里做DE/RE GPIO初始化、DMA接收启动
    // HAL_UART_Receive_DMA(s_huart, rx_buf, RX_BUF_MAX);
    return RS485_OK;
}

void BSP_RS485_SetRxCpltCallback(Rs485RxCpltCallback cb)
{
    g_rx_cplt_cb = cb;
}

void BSP_RS485_SetTxCpltCallback(Rs485TxCpltCallback cb)
{
    g_tx_cplt_cb = cb;
}
/*----------------------------------------------------------------------------------------*/
// 中断回调钩子（HAL库的串口完成回调）
// void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
// {
//     if(huart == s_huart && g_rx_cplt_cb != NULL)
//     {
//         //g_rx_cplt_cb();
//     }
// }

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if(huart == s_huart && g_tx_cplt_cb != NULL)
    {
        g_tx_cplt_cb();
    }
}

/**
  * @brief  不定长空闲中断接收完成回调函数
  * @param  huart: pointer to a UART_HandleTypeDef structure
  * @retval None
  */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if(s_huart == huart)
    {
        // 调用上层回调函数
        g_rx_cplt_cb(Size);
        // 重新启动DMA接收
        HAL_UART_Receive_DMA(s_huart, mb_handle->dma_rx_buf, MODBUS_FRAME_MAX_LEN);
    }
}