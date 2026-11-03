#ifndef RTC_SD3078_H
#define RTC_SD3078_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * SD3078 高精度外部 RTC 驱动。
 * 硬件：I2C1，7位地址 0x32（按 SD3078 数据手册）。
 * 依赖：CubeMX 已生成 MX_I2C1_Init()，全局句柄 hi2c1 可用。
 *
 * 时间结构体：BCD 编码与十进制互相转换由本驱动内部完成。
 */

// RTC 时间（2000-01-01 ~ 2099-12-31）
typedef struct
{
    uint16_t year;   // 完整年份，如 2026
    uint8_t  month;  // 1 ~ 12
    uint8_t  day;    // 1 ~ 31
    uint8_t  hour;   // 0 ~ 23
    uint8_t  minute; // 0 ~ 59
    uint8_t  second; // 0 ~ 59
    uint8_t  weekday; // 0 ~ 6，周日=0
} rtc_datetime_t;

// 初始化：检查 SD3078 是否在线，若停止运行则重新启动
HAL_StatusTypeDef rtc_sd3078_init(void);

// 读取当前时间
HAL_StatusTypeDef rtc_sd3078_get_time(rtc_datetime_t *dt);

// 设置时间
HAL_StatusTypeDef rtc_sd3078_set_time(const rtc_datetime_t *dt);

// 判断 RTC 是否在走（停止标志位）
bool rtc_sd3078_is_running(void);

// 将 RTC 时间转为 Unix 时间戳（秒，UTC，2000-01-01 起）
uint32_t rtc_sd3078_to_unix(const rtc_datetime_t *dt);

// 将 Unix 时间戳转为 RTC 时间
void rtc_sd3078_from_unix(uint32_t unix_s, rtc_datetime_t *dt);

#endif /* RTC_SD3078_H */
