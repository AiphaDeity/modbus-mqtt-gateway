/*
 * SD3078时间读写及UTC Unix秒换算；I2C互斥由调用方持有。
 * 初始化只探测，不清除上电/停振标志；显式校时成功后才清除停振标志。
 */
#include "rtc_sd3078.h"
#include "stm32f4xx_hal_i2c.h"

extern I2C_HandleTypeDef hi2c1;

#define SD3078_I2C_ADDR  (0x32U << 1)
#define SD3078_REG_SEC   0x00U
#define SD3078_REG_WEEK  0x03U
#define SD3078_REG_DAY   0x04U
#define SD3078_REG_CTRL1 0x0FU
#define SD3078_REG_CTRL2 0x10U
#define SD3078_WRTC1    0x80U
#define SD3078_WRTC2    0x04U
#define SD3078_WRTC3    0x80U
#define SD3078_OSF      0x40U
#define SD3078_RTCF     0x01U

#define BCD2DEC(b) (((b) >> 4) * 10U + ((b) & 0x0FU))
#define DEC2BCD(d) ((((d) / 10U) << 4) | ((d) % 10U))

static const uint8_t s_month_days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

// 读取单个寄存器，HAL使用左移一位的7位器件地址。
static HAL_StatusTypeDef sd3078_read_reg(uint8_t reg, uint8_t *val)
{
    return HAL_I2C_Mem_Read(&hi2c1, SD3078_I2C_ADDR, reg,
                           I2C_MEMADD_SIZE_8BIT, val, 1, 100);
}

// 只修改指定控制位，保留报警、电源和其他控制状态。
static HAL_StatusTypeDef sd3078_update_bits(uint8_t reg, uint8_t clear, uint8_t set)
{
    uint8_t value;
    HAL_StatusTypeDef st = sd3078_read_reg(reg, &value);
    if (st != HAL_OK) return st;
    value = (uint8_t)((value & ~clear) | set);
    return HAL_I2C_Mem_Write(&hi2c1, SD3078_I2C_ADDR, reg,
                            I2C_MEMADD_SIZE_8BIT, &value, 1, 100);
}

// 按WRTC1、WRTC2、WRTC3顺序解锁，失败由校时调用方统一重新上锁。
static HAL_StatusTypeDef sd3078_unlock_write(void)
{
    HAL_StatusTypeDef st = sd3078_update_bits(SD3078_REG_CTRL2, 0, SD3078_WRTC1);
    if (st == HAL_OK) st = sd3078_update_bits(SD3078_REG_CTRL1, 0, SD3078_WRTC2);
    if (st == HAL_OK) st = sd3078_update_bits(SD3078_REG_CTRL1, 0, SD3078_WRTC3);
    return st;
}

// 先清WRTC2/3再清WRTC1；第一步失败也尝试关闭外层写允许。
static HAL_StatusTypeDef sd3078_lock_write(void)
{
    HAL_StatusTypeDef st = sd3078_update_bits(SD3078_REG_CTRL1, SD3078_WRTC2 | SD3078_WRTC3, 0);
    HAL_StatusTypeDef outer = sd3078_update_bits(SD3078_REG_CTRL2, SD3078_WRTC1, 0);
    return st == HAL_OK ? outer : st;
}

// 校验RTC可表示的公历日期；2000～2099年内闰年可按4整除判断。
static bool sd3078_valid_date(const rtc_datetime_t *dt)
{
    if (dt == NULL || dt->year < 2000U || dt->year > 2099U ||
        dt->month < 1U || dt->month > 12U || dt->day < 1U ||
        dt->hour > 23U || dt->minute > 59U || dt->second > 59U || dt->weekday > 6U)
        return false;
    uint8_t days = s_month_days[dt->month - 1U];
    if (dt->month == 2U && dt->year % 4U == 0U) days++;
    return dt->day <= days;
}

// 只探测控制寄存器；不能通过初始化写操作抹去时间失效证据。
HAL_StatusTypeDef rtc_sd3078_init(void)
{
    uint8_t status;
    return sd3078_read_reg(SD3078_REG_CTRL1, &status);
}

// 一次连续读取得到锁存时间，拒绝非法BCD/日期并正确换算12小时制。
HAL_StatusTypeDef rtc_sd3078_get_time(rtc_datetime_t *dt)
{
    if (dt == NULL) return HAL_ERROR;
    uint8_t buf[7];
    HAL_StatusTypeDef st = HAL_I2C_Mem_Read(&hi2c1, SD3078_I2C_ADDR,
                                          SD3078_REG_SEC, I2C_MEMADD_SIZE_8BIT,
                                          buf, sizeof(buf), 100);
    if (st != HAL_OK) return st;

    const uint8_t mask[7] = {0x7F, 0x7F, (buf[2] & 0x80U) ? 0x3F : 0x1F, 0x07, 0x3F, 0x1F, 0xFF};
    for (uint32_t i = 0U; i < sizeof(buf); i++) {
        uint8_t value = buf[i] & mask[i];
        if (i != SD3078_REG_WEEK && ((value & 0x0FU) > 9U || (value >> 4U) > 9U))
            return HAL_ERROR;
    }

    rtc_datetime_t value = {
        .year = (uint16_t)(2000U + BCD2DEC(buf[6])),
        .month = (uint8_t)BCD2DEC(buf[5] & 0x1FU),
        .day = (uint8_t)BCD2DEC(buf[SD3078_REG_DAY] & 0x3FU),
        .hour = (uint8_t)BCD2DEC(buf[2] & mask[2]),
        .minute = (uint8_t)BCD2DEC(buf[1] & 0x7FU),
        .second = (uint8_t)BCD2DEC(buf[0] & 0x7FU),
        .weekday = buf[SD3078_REG_WEEK] & 0x07U
    };
    if ((buf[2] & 0x80U) == 0U) {
        if (value.hour < 1U || value.hour > 12U) return HAL_ERROR;
        value.hour = (uint8_t)(value.hour % 12U + ((buf[2] & 0x20U) ? 12U : 0U));
    }
    if (!sd3078_valid_date(&value)) return HAL_ERROR;
    *dt = value;
    return HAL_OK;
}

// 显式写入完整7字节时间并使用24小时制，所有失败路径均尝试重新上锁。
HAL_StatusTypeDef rtc_sd3078_set_time(const rtc_datetime_t *dt)
{
    if (!sd3078_valid_date(dt)) return HAL_ERROR;
    uint8_t buf[7] = {
        DEC2BCD(dt->second), DEC2BCD(dt->minute), DEC2BCD(dt->hour) | 0x80U,
        dt->weekday, DEC2BCD(dt->day), DEC2BCD(dt->month), DEC2BCD(dt->year - 2000U)
    };
    HAL_StatusTypeDef st = sd3078_unlock_write();
    if (st == HAL_OK)
        st = HAL_I2C_Mem_Write(&hi2c1, SD3078_I2C_ADDR, SD3078_REG_SEC,
                              I2C_MEMADD_SIZE_8BIT, buf, sizeof(buf), 100);
    if (st == HAL_OK) {
        rtc_datetime_t verify;
        st = rtc_sd3078_get_time(&verify);
        uint32_t expected = rtc_sd3078_to_unix(dt);
        uint32_t actual = st == HAL_OK ? rtc_sd3078_to_unix(&verify) : 0U;
        if (actual < expected || actual - expected > 1U) st = HAL_ERROR;
    }
    if (st == HAL_OK)
        st = sd3078_update_bits(SD3078_REG_CTRL1, SD3078_OSF, 0);
    HAL_StatusTypeDef locked = sd3078_lock_write();
    return st == HAL_OK ? locked : st;
}

// 无上电/历史停振标志才可采用RTC值；不把秒寄存器的保留位当作运行标志。
bool rtc_sd3078_is_running(void)
{
    uint8_t status;
    return sd3078_read_reg(SD3078_REG_CTRL1, &status) == HAL_OK &&
           (status & (SD3078_OSF | SD3078_RTCF)) == 0U;
}

// 将2000～2099公历时间转为从1970年起的UTC Unix秒；非法日期返回0。
uint32_t rtc_sd3078_to_unix(const rtc_datetime_t *dt)
{
    if (!sd3078_valid_date(dt)) return 0U;
    uint32_t days = 0U;
    for (uint32_t year = 2000U; year < dt->year; year++)
        days += year % 4U == 0U ? 366U : 365U;
    for (uint32_t month = 1U; month < dt->month; month++)
        days += s_month_days[month - 1U] + (month == 2U && dt->year % 4U == 0U ? 1U : 0U);
    days += dt->day - 1U;
    return RTC_SD3078_UNIX_MIN + days * 86400U +
           dt->hour * 3600U + dt->minute * 60U + dt->second;
}

// 将UTC Unix秒转为RTC日期；超出芯片范围时清零输出，weekday按周日为0计算。
void rtc_sd3078_from_unix(uint32_t unix_s, rtc_datetime_t *dt)
{
    if (dt == NULL) return;
    *dt = (rtc_datetime_t){0};
    if (unix_s < RTC_SD3078_UNIX_MIN || unix_s > RTC_SD3078_UNIX_MAX) return;
    uint32_t seconds = unix_s - RTC_SD3078_UNIX_MIN;
    uint32_t days = seconds / 86400U;
    uint32_t remainder = seconds % 86400U;
    dt->hour = (uint8_t)(remainder / 3600U);
    dt->minute = (uint8_t)(remainder % 3600U / 60U);
    dt->second = (uint8_t)(remainder % 60U);
    dt->weekday = (uint8_t)((unix_s / 86400U + 4U) % 7U);
    uint16_t year = 2000U;
    while (days >= (year % 4U == 0U ? 366U : 365U)) {
        days -= year % 4U == 0U ? 366U : 365U;
        year++;
    }
    dt->year = year;
    uint32_t month = 1U;
    while (days >= s_month_days[month - 1U] + (month == 2U && year % 4U == 0U ? 1U : 0U)) {
        days -= s_month_days[month - 1U] + (month == 2U && year % 4U == 0U ? 1U : 0U);
        month++;
    }
    dt->month = (uint8_t)month;
    dt->day = (uint8_t)(days + 1U);
}
