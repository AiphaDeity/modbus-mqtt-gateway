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
 * I2C接口由调用方持有总线互斥锁；时间换算函数不访问硬件。
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

// 初始化只探测；不清除上电/停振标志，也不自动设置时间。
HAL_StatusTypeDef rtc_sd3078_init(void);

// 读取当前时间
HAL_StatusTypeDef rtc_sd3078_get_time(rtc_datetime_t *dt);

// 完整写入24小时制时间，读回一致才清停振标志，返回前尝试重新上锁。
HAL_StatusTypeDef rtc_sd3078_set_time(const rtc_datetime_t *dt);

// 检查CTR1的上电/停振标志均已清除；I/O失败返回false。
bool rtc_sd3078_is_running(void);

#define RTC_SD3078_UNIX_MIN 946684800U
#define RTC_SD3078_UNIX_MAX 4102444799U

// UTC Unix秒从1970年起；仅接受2000～2099年，非法日期返回0。
uint32_t rtc_sd3078_to_unix(const rtc_datetime_t *dt);

// 将UTC Unix秒转为RTC时间；越界清零输出，调用方不可据此写入。
void rtc_sd3078_from_unix(uint32_t unix_s, rtc_datetime_t *dt);

#endif /* RTC_SD3078_H */
