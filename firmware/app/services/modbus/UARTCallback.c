/*
 * UARTCallback.c
 *
 *  Created on: May 27, 2020
 *      Author: Alejandro Mera
 */

#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "task.h"
#include "main.h"
#include "Modbus.h"


/**
 * @brief
 * This is the callback for HAL interrupts of UART TX used by Modbus library.
 * This callback is shared among all UARTS, if more interrupts are used
 * user should implement the correct control flow and verification to maintain
 * Modbus functionality.
 * @ingroup UartHandle UART HAL handler
 */

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	/* Modbus RTU TX callback BEGIN */
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	int i;
	for (i = 0; i < numberHandlers; i++ )
	{
	   	if (mHandlers[i]->port == huart  )
	   	{
	   		/*
	   		 * ★在 TC 中断里立刻拉低 RS485 方向脚（微秒级）。
	   		 *
	   		 * 从站收完帧 1~2ms 就会回包，方向脚晚 2ms 拉低，应答就会撞在
	   		 * 我们还在驱动的总线上，直接被丢掉。放这里才能保证时序。
	   		 *
	   		 * 注意：不要在这里给 StartTaskModbusMaster 发通知！
	   		 * 合法的通知来源只有 T35 回调和 Timeout 回调，多一个来源会让
	   		 * master 在应答到达前就被唤醒，getRxBuffer 拿到空帧。
	   		 */
	   		if (mHandlers[i]->EN_Port != NULL)
	   		{
	   			HAL_GPIO_WritePin(mHandlers[i]->EN_Port, mHandlers[i]->EN_Pin, GPIO_PIN_RESET);
	   		}
	   		break;
	   	}

	}
	portYIELD_FROM_ISR( xHigherPriorityTaskWoken );

	/* Modbus RTU TX callback END */

	/*
	 * Here you should implement the callback code for other UARTs not used by Modbus
	 *
	 * */

}



/**
 * @brief
 * This is the callback for HAL interrupt of UART RX
 * This callback is shared among all UARTS, if more interrupts are used
 * user should implement the correct control flow and verification to maintain
 * Modbus functionality.
 * @ingroup UartHandle UART HAL handler
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *UartHandle)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	/* Modbus RTU RX callback BEGIN */
    int i;
    for (i = 0; i < numberHandlers; i++ )
    {
    	if (mHandlers[i]->port == UartHandle  )
    	{

    		if(mHandlers[i]->xTypeHW == USART_HW)
    		{
    			RingAdd(&mHandlers[i]->xBufferRX, mHandlers[i]->dataRX);
    			HAL_UART_Receive_IT(mHandlers[i]->port, &mHandlers[i]->dataRX, 1);
    			xTimerResetFromISR(mHandlers[i]->xTimerT35, &xHigherPriorityTaskWoken);
    		}
    		break;
    	}
    }
    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );

	/* Modbus RTU RX callback END */

	/*
	 * Here you should implement the callback code for other UARTs not used by Modbus
	 *
	 *
	 * */

}


/**
 * @brief HAL UART 错误回调 —— 不管用不用 DMA 都必须实现！
 *
 * STM32F4 的 USART SR 里的错误标志（ORE/FE/NE/PE）需要按
 * "先读 SR，再读 DR" 的特定顺序才能清除。
 *
 * HAL_UART_IRQHandler 只要看到 SR 里有任何错误标志挂着，就会：
 *   1. 调 UART_EndRxTransfer_IT() → 把 RxState 设回 READY，关 RXNEIE
 *   2. 调 HAL_UART_ErrorCallback()
 *
 * 本回调做两件事：
 *   1. 按"先读 SR，再读 DR"清掉所有 USART 错误标志
 *   2. 立刻重新武装 RX IT
 *
 * 第 2 步是必须的：HAL 在调本回调前已经执行 UART_EndRxTransfer_IT()
 * （RxState=READY、RXNEIE=0）。而 getRxBuffer() 只有等 T35 定时器超时后才会
 * 被调用，一个字节都收不到时 T35 永远不会 fire —— 不在这里重启，RX 中断就
 * 永远关着，后续所有从站应答都会被丢弃（表现为 rxneie=0 / 每帧都 1200ms 超时）。
 *
 * HAL_UART_Receive_IT 只检查 RxState（此时已是 READY），不会因 gState 处于
 * BUSY_TX 而返回 HAL_BUSY，可在 ISR 中安全调用。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    int i;
    for (i = 0; i < numberHandlers; i++)
    {
        if (mHandlers[i]->port == huart)
        {
            /* F4 参考手册规定：清 ORE/FE/NE/PE 的正确顺序是
             * 先读 SR，再读 DR。单独写 0 到 SR 寄存器无效！ */
            volatile uint32_t sr_dummy = huart->Instance->SR;
            volatile uint32_t dr_dummy = huart->Instance->DR;
            (void)sr_dummy;
            (void)dr_dummy;

            /* 错误已清，重新启动单字节 RX IT，恢复接收链路 */
            if (mHandlers[i]->xTypeHW == USART_HW &&
                huart->RxState == HAL_UART_STATE_READY)
            {
                HAL_UART_Receive_IT(huart, &mHandlers[i]->dataRX, 1);
            }
            break;
        }
    }
}

#if  ENABLE_USART_DMA ==  1
/*
 * DMA requires to handle callbacks for special communication modes of the HAL
 * It also has to handle eventual errors including extra steps that are not automatically
 * handled by the HAL
 * */


void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
	    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		/* Modbus RTU RX callback BEGIN */
	    int i;
	    for (i = 0; i < numberHandlers; i++ )
	    {
	    	if (mHandlers[i]->port == huart  )
	    	{


	    		if(mHandlers[i]->xTypeHW == USART_HW_DMA)
	    		{
	    			if(Size) //check if we have received any byte
	    			{
		    				mHandlers[i]->xBufferRX.u8available = Size;
		    				mHandlers[i]->xBufferRX.overflow = false;

		    				while(HAL_UARTEx_ReceiveToIdle_DMA(mHandlers[i]->port, mHandlers[i]->xBufferRX.uxBuffer, MAX_BUFFER) != HAL_OK)
		    				{
		    					HAL_UART_DMAStop(mHandlers[i]->port);

		    				}
		    				__HAL_DMA_DISABLE_IT(mHandlers[i]->port->hdmarx, DMA_IT_HT); // we don't need half-transfer interrupt

		    				xTaskNotifyFromISR(mHandlers[i]->myTaskModbusAHandle, 0 , eSetValueWithOverwrite, &xHigherPriorityTaskWoken);
	    			}
	    		}

	    		break;
	    	}
	    }
	    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
}

#endif
