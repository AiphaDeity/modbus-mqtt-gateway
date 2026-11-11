/*
 * storage_time.c — 时间服务实现
 *
 * 给所有模块提供统一的 Unix 时间戳 API。
 * 底层依赖 BSP 层的 rtc_sd3078 驱动。
 * I2C1 总线访问由 g_m3_i2c1_mutex 互斥锁保护。
 */

#include "storage_runtime.h"
#include "storage_time.h"
#include "storage.h"        // g_m3_i2c1_mutex
#include "rtc_sd3078.h"     // BSP RTC 驱动
#include "storage_timefloor.h"
#include "cmsis_os2.h"

uint32_t storage_get_unix_time(void)
{
    rtc_datetime_t dt;

    // I2C1 总线加锁
    if (osMutexAcquire(g_m3_i2c1_mutex, (osKernelGetTickFreq() + 9U) / 10U) != osOK)
        return 0;

    HAL_StatusTypeDef st = rtc_sd3078_get_time(&dt);

    osMutexRelease(g_m3_i2c1_mutex);

    if (st != HAL_OK)
        return 0;

    return rtc_sd3078_to_unix(&dt);
}

static HAL_StatusTypeDef storage_set_unix_time_impl(uint32_t unix_s)
{
    rtc_datetime_t dt;

    rtc_sd3078_from_unix(unix_s, &dt);

    if (osMutexAcquire(g_m3_i2c1_mutex, (osKernelGetTickFreq() + 9U) / 10U) != osOK)
        return HAL_ERROR;

    HAL_StatusTypeDef st = rtc_sd3078_set_time(&dt);

    osMutexRelease(g_m3_i2c1_mutex);

    if (st != HAL_OK)
        return st;

    // 校时成功后推进持久时间下界（未达阈值时内部自动跳过）
    return storage_timefloor_update(unix_s);
}

bool storage_time_is_valid(void)
{
    rtc_datetime_t dt;
    uint32_t now = 0;
    bool running = false;

    if (osMutexAcquire(g_m3_i2c1_mutex, (osKernelGetTickFreq() + 9U) / 10U) != osOK)
        return false;

    running = rtc_sd3078_is_running();
    if (running && rtc_sd3078_get_time(&dt) == HAL_OK)
        now = rtc_sd3078_to_unix(&dt);

    osMutexRelease(g_m3_i2c1_mutex);

    // RTC 在走 + 时间不早于持久下界，才算可信
    return running && storage_timefloor_trusted(now);
}

/* 在门控内校时并同步持久时间下界。 */
HAL_StatusTypeDef storage_set_unix_time(uint32_t unix_s)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_set_unix_time_impl(unix_s);
    storage_mutation_end();
    return st;
}
