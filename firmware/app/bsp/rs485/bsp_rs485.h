#ifndef BSP_RS485_H
#define BSP_RS485_H

#include "main.h"
#include <stdint.h>

typedef enum
{
    RS485_OK = 0,
    RS485_ERR_TIMEOUT,      // 超时错误
    RS485_ERR_BUSY,         // 忙错误
    RS485_ERR_PARITY,       // 校验错误
    RS485_ERR_OVERRUN       // 溢出错误
} Rs485Status_e;

typedef enum
{
    RS485_DIR_RX = 0,
    RS485_DIR_TX
} Rs485Dir_t;

// 回调类型
typedef void (*Rs485RxCpltCallback)(uint16_t len);
typedef void (*Rs485TxCpltCallback)(void);

// ✅ 重点：Init带UART句柄指针参数
Rs485Status_e BSP_RS485_Init(UART_HandleTypeDef *huart);
void BSP_RS485_SetDir(Rs485Dir_t dir);
Rs485Status_e BSP_RS485_SendFrame(const uint8_t *data, uint16_t len);
Rs485Status_e BSP_RS485_ReadFrame(uint8_t *dst, uint16_t *p_len);

void BSP_RS485_SetRxCpltCallback(Rs485RxCpltCallback cb);
void BSP_RS485_SetTxCpltCallback(Rs485TxCpltCallback cb);
void BSP_RS485_RxFlush(void);

#endif