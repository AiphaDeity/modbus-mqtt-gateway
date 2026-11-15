#ifndef STORAGE_TIME_H
#define STORAGE_TIME_H

#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * 时间服务 —— 给所有模块提供统一的时间 API。
 * 底层依赖 SD3078 RTC（BSP 层 rtc_sd3078）。
 */

#define STORAGE_TIME_MIN_UNIX 1577836800U

// 仅任务上下文，可阻塞等待I2C；UTC Unix秒从1970年起，无效/回拨/读失败返回0。
uint32_t storage_get_unix_time(void);

// Storage_Task显式校时，限2020～2099年且不早于已恢复下界；写入、上锁、读回及下界保存成功才返回OK。
HAL_StatusTypeDef storage_set_unix_time(uint32_t unix_s);

// 判断 RTC 是否在走（时间是否可信）
bool storage_time_is_valid(void);

#endif /* STORAGE_TIME_H */
