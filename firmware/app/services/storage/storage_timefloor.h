#ifndef STORAGE_TIMEFLOOR_H
#define STORAGE_TIMEFLOOR_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * 持久时间下界（control_time_floor）。
 *
 * 作用：把"设备曾见过的最新时间"持久化到 Flash，作为时间可信度的下界。
 * 若启动后 RTC 读出时间 < 下界，说明 RTC 掉电/被回拨，时间不可信，
 * 上层不应使用该时间打时间戳，应等待校时。
 *
 * 位置：身份区 0x044000 / 0x045000（双份冗余，写旧副本）。
 * 写入频率：仅当当前时间比已存下界前进超过 STORAGE_TIMEFLOOR_STEP_SEC 时才落盘，
 *           避免频繁擦写 Flash。
 */

#define STORAGE_TIMEFLOOR_STEP_SEC  3600U   /* 每小时最多落盘一次 */

// 初始化：读双份，取较大的有效值
HAL_StatusTypeDef storage_timefloor_init(void);

// 当前持久下界（0 表示尚无参考值）
uint32_t storage_timefloor_get(void);

// 若 now 比已存下界前进超过阈值则落盘；now 小于下界时不会下调
HAL_StatusTypeDef storage_timefloor_update(uint32_t now);

// 初始化成功后判断给定UTC秒不早于下界；初始化失败/未完成返回false，合法空白下界不单独否定时间。
bool storage_timefloor_trusted(uint32_t now);

#endif /* STORAGE_TIMEFLOOR_H */
