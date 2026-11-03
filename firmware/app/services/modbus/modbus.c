#include "modbus.h"
#include "bsp_rs485.h"
#include <stdint.h>
#include "modbus_app.h"



extern mb_handle_t *mb_handle;
osMessageQueueId_t modbus_tx_queue = NULL;

#define MODBUS_TX_QUEUE_DEPTH  8U
#define MODBUS_TX_ITEM_SIZE    64U

AppModuleInitResult Modbus_Init(void)
{
    if (BSP_RS485_Init(&MODBUS_RS485_UART) != 0)
    {
        return APP_MODULE_INIT_FAILED;
    }

    if (mb_handle_init() != MB_STATUS_OK)
    {
        return APP_MODULE_INIT_FAILED;
    }

    // ✅ 在这里注册接收完成回调
    BSP_RS485_SetRxCpltCallback(Modbus_Rs485FrameReceivedCb);
    BSP_RS485_SetTxCpltCallback(Modbus_Tx485FrameSentCb);
    // 如果需要发送完成回调，也在这里注册
    // BSP_RS485_SetTxCpltCallback();


    // 发送队列，保留之前的帧
    modbus_tx_queue = osMessageQueueNew(MODBUS_TX_QUEUE_DEPTH, MODBUS_TX_ITEM_SIZE, NULL);
    if (modbus_tx_queue == NULL)
    {
        return APP_MODULE_INIT_FAILED;
    }

    return APP_MODULE_INIT_OK;
}

void vModbusPollTask(void *arg)
{
    (void)arg;
    uint8_t frame_buf[MODBUS_TX_ITEM_SIZE] = {0};
    osStatus_t ret;

    while (1)
    {
        // 等待新帧通知
        ret = osSemaphoreAcquire(mb_handle->xNewFrameNotifySem, osWaitForever);
        if (ret == osOK)
        {
            
            // 拿到帧，解析         
            mb_parse_frame(&mb_handle->frame_slot);



            // ========== 读/写运行寄存器数据，拿 slave_data_mutex ==========
            if (osMutexAcquire(mb_handle->xSlaveDataMutex, 10) == osOK)
            {
                // 访问、修改寄存器、在线状态
                // ...
                osMutexRelease(mb_handle->xSlaveDataMutex);
            }

            // ========== 读取/修改配置，拿 slave_cfg_mutex ==========
            if (osMutexAcquire(slave_cfg_mutex, 10) == osOK)
            {
                // 读取/修改 slave_id、采集间隔、使能标志
                // ...
                osMutexRelease(slave_cfg_mutex);
            }

            // 组应答帧，丢发送队列
        }
    }
}

/*
 * @brief  接收完成回调函数
 * @param  rx_len: 接收字节数
 * @retval None
*/
static void Modbus_Rs485FrameReceivedCb(uint16_t rx_len)
{
    // 数据入槽
    mb_in_slot(rx_len);

    

    // 通知解析任务有新帧
    osSemaphoreRelease(mb_handle->xNewFrameNotifySem);
}

static void Modbus_Tx485FrameSentCb(void)
{
    // 通发送完成
    mb_handle->state = MB_STATE_WAIT_ACK;               // 等待应答
    mb_handle->send_tick = osKernelGetTickCount();      // 记录发送时间
}
