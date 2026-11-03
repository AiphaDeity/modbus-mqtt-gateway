#ifndef STORAGE_TIME_H
#define STORAGE_TIME_H

#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * 时间服务 —— 给所有模块提供统一的时间 API。
 * 底层依赖 SD3078 RTC（BSP 层 rtc_sd3078）。
 */

// 获取当前 Unix 时间戳（秒，从 2000-01-01 00:00:00 起）
// 失败返回 0
uint32_t storage_get_unix_time(void);

// 设置当前 Unix 时间戳
HAL_StatusTypeDef storage_set_unix_time(uint32_t unix_s);

// 判断 RTC 是否在走（时间是否可信）
bool storage_time_is_valid(void);

#endif /* STORAGE_TIME_H */
