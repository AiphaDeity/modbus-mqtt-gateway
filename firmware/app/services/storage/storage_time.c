/*
 * RTC时间与持久下界共同决定可信性；I2C1只在原互斥锁内访问。
 */
#include "storage_runtime.h"
#include "storage_time.h"
#include "storage.h"
#include "rtc_sd3078.h"
#include "storage_timefloor.h"
#include "cmsis_os2.h"

// 同一次受锁读取核对RTC状态与日期；未知、回拨或底层失败均返回0。
uint32_t storage_get_unix_time(void)
{
    if (g_m3_i2c1_mutex == NULL ||
        osMutexAcquire(g_m3_i2c1_mutex, (osKernelGetTickFreq() + 9U) / 10U) != osOK)
        return 0U;
    rtc_datetime_t dt;
    bool valid = rtc_sd3078_is_running() && rtc_sd3078_get_time(&dt) == HAL_OK;
    osMutexRelease(g_m3_i2c1_mutex);
    uint32_t now = valid ? rtc_sd3078_to_unix(&dt) : 0U;
    return now >= STORAGE_TIME_MIN_UNIX && storage_timefloor_trusted(now) ? now : 0U;
}

// 拒绝未恢复下界或向下回拨；写入并读回RTC后再推进持久下界。
static HAL_StatusTypeDef storage_set_unix_time_impl(uint32_t unix_s)
{
    if (unix_s < STORAGE_TIME_MIN_UNIX || unix_s > RTC_SD3078_UNIX_MAX ||
        g_m3_i2c1_mutex == NULL || !storage_timefloor_trusted(unix_s)) return HAL_ERROR;
    rtc_datetime_t dt;
    rtc_sd3078_from_unix(unix_s, &dt);
    if (osMutexAcquire(g_m3_i2c1_mutex, (osKernelGetTickFreq() + 9U) / 10U) != osOK)
        return HAL_ERROR;
    HAL_StatusTypeDef st = rtc_sd3078_set_time(&dt);
    if (st == HAL_OK && !rtc_sd3078_is_running()) st = HAL_ERROR;
    osMutexRelease(g_m3_i2c1_mutex);
    return st == HAL_OK ? storage_timefloor_update(unix_s) : st;
}

// 复用同一可信UTC判定，不维护第二套RTC有效性规则。
bool storage_time_is_valid(void)
{
    return storage_get_unix_time() != 0U;
}

// 仅Storage_Task在变更门控内显式校时；回拨不会降低持久下界。
HAL_StatusTypeDef storage_set_unix_time(uint32_t unix_s)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_set_unix_time_impl(unix_s);
    storage_mutation_end();
    return st;
}
