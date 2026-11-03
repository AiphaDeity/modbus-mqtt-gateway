#ifndef MODBUS_H
#define MODBUS_H

#include "app_modules.h"
#include "cmsis_os2.h"
#include "main.h"

// 告诉编译器 huart3 在别的文件定义
extern UART_HandleTypeDef huart3;
#define MODBUS_RS485_UART        huart3

// 发送队列，保留之前的
extern osMessageQueueId_t modbus_tx_queue;

static void Modbus_Rs485FrameReceivedCb(uint16_t rx_len);
static void Modbus_Tx485FrameSentCb(void);


#endif