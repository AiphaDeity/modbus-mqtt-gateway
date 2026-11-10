/*这是 SD3078 时钟芯片的 BSP 驱动底层代码，主要定义了芯片的 I2C 地址、寄存器编号，还有 BCD 码转换宏，用来给上层提供读取和设置时间的接口。
 * rtc_sd3078.c — SD3078 外部 RTC 驱动实现
 *
 * 硬件：I2C1 (PB6=SCL, PB7=SDA)，7位地址 0x32
 * 依赖：CubeMX 已生成 MX_I2C1_Init()，全局句柄 hi2c1
 *
 * SD3078 时间寄存器（BCD 编码）：
 *   0x00 秒   0x01 分   0x02 时   0x03 日
 *   0x04 周   0x05 月   0x06 年   （年 = 实际年份 - 2000）
 * 控制寄存器：
 *   0x0F 写使能控制   0x10 写保护解锁
 */

#include "rtc_sd3078.h"
#include "stm32f4xx_hal_i2c.h"

extern I2C_HandleTypeDef hi2c1;   // CubeMX 在 i2c.c 里定义的全局句柄

#define SD3078_I2C_ADDR_WR  (0x32 << 1)  // 0x64  写地址
#define SD3078_I2C_ADDR_RD  ((0x32 << 1) | 0x01)  // 0x65  读地址

#define SD3078_REG_SEC      0x00
#define SD3078_REG_MIN      0x01
#define SD3078_REG_HOUR     0x02
#define SD3078_REG_DAY      0x03
#define SD3078_REG_WEEK     0x04
#define SD3078_REG_MONTH    0x05
#define SD3078_REG_YEAR     0x06
#define SD3078_REG_WRTC     0x0F   // 写使能控制寄存器
#define SD3078_REG_WRTC2    0x10   // 写保护解锁寄存器

#define BCD2DEC(b)  (((b) >> 4) * 10 + ((b) & 0x0F))
#define DEC2BCD(d)  ((((d) / 10) << 4) | ((d) % 10))

/* ---------- 内部工具函数 ---------- */

// 向单个寄存器写 1 字节
//&hi2c1：IIC外设句柄；SD3078_I2C_ADDR_W：芯片的8位写地址
//reg：芯片内部的寄存器地址；I2C_MEMADD_SIZE_8BIT：地址字节大小为8位
//val：要写入的字节值
static HAL_StatusTypeDef sd3078_write_reg(uint8_t reg, uint8_t val)
{
    return HAL_I2C_Mem_Write(&hi2c1, SD3078_I2C_ADDR_WR,
                             reg, I2C_MEMADD_SIZE_8BIT,
                             &val, 1, 100);
}

// 从单个寄存器读 1 字节
static HAL_StatusTypeDef sd3078_read_reg(uint8_t reg, uint8_t *val)
{
    return HAL_I2C_Mem_Read(&hi2c1, SD3078_I2C_ADDR_RD,
                            reg, I2C_MEMADD_SIZE_8BIT,
                            val, 1, 100);
}

// 解锁写保护，允许修改时间寄存器
static HAL_StatusTypeDef sd3078_unlock_write(void)
{
    // 1) 向 0x10 写 0x80 解锁写保护
    if (sd3078_write_reg(SD3078_REG_WRTC2, 0x80) != HAL_OK)
        return HAL_ERROR;
    // 2) 向 0x0F 写 0x84 使能写并启动 RTC
    return sd3078_write_reg(SD3078_REG_WRTC, 0x84);
}

// 重新上锁，恢复写保护
static HAL_StatusTypeDef sd3078_lock_write(void)
{
    return sd3078_write_reg(SD3078_REG_WRTC, 0x00);
}

/* ---------- 对外 API ---------- */

HAL_StatusTypeDef rtc_sd3078_init(void)
{
    uint8_t reg;
    HAL_StatusTypeDef st;

    // 探测：读秒寄存器，成功说明 I2C 通了
    st = sd3078_read_reg(SD3078_REG_SEC, &reg);
    if (st != HAL_OK)
        return HAL_ERROR;   // 芯片不在线

    // 如果 RTC 停了（STOP 标志），重启
    if (!rtc_sd3078_is_running()) {
        st = sd3078_unlock_write();
        if (st != HAL_OK) return HAL_ERROR;
        // 重新上锁
        sd3078_lock_write();
    }
    return HAL_OK;
}

HAL_StatusTypeDef rtc_sd3078_get_time(rtc_datetime_t *dt)
{
    uint8_t buf[7];
    HAL_StatusTypeDef st;

    if (dt == NULL)
        return HAL_ERROR;

    // 连续读 0x00~0x06 共 7 个寄存器
    st = HAL_I2C_Mem_Read(&hi2c1, SD3078_I2C_ADDR_RD,
                          SD3078_REG_SEC, I2C_MEMADD_SIZE_8BIT,
                          buf, 7, 100);
    if (st != HAL_OK)
        return HAL_ERROR;

    // BCD → 十进制
    dt->second  = BCD2DEC(buf[0] & 0x7F);
    dt->minute  = BCD2DEC(buf[1] & 0x7F);
    dt->hour    = BCD2DEC(buf[2] & 0x3F);
    dt->day     = BCD2DEC(buf[3] & 0x3F);
    dt->weekday = buf[4] & 0x07;
    dt->month   = BCD2DEC(buf[5] & 0x1F);
    dt->year    = 2000 + BCD2DEC(buf[6]);

    return HAL_OK;
}

HAL_StatusTypeDef rtc_sd3078_set_time(const rtc_datetime_t *dt)
{
    uint8_t buf[7];
    HAL_StatusTypeDef st;

    if (dt == NULL)
        return HAL_ERROR;

    // 十进制 → BCD
    buf[0] = DEC2BCD(dt->second)  & 0x7F;
    buf[1] = DEC2BCD(dt->minute)  & 0x7F;
    buf[2] = DEC2BCD(dt->hour)    & 0x3F;
    buf[3] = DEC2BCD(dt->day)     & 0x3F;
    buf[4] = dt->weekday & 0x07;
    buf[5] = DEC2BCD(dt->month)   & 0x1F;
    buf[6] = DEC2BCD(dt->year - 2000);

    // 解锁写保护
    st = sd3078_unlock_write();
    if (st != HAL_OK)
        return HAL_ERROR;

    // 连续写 7 个时间寄存器
    st = HAL_I2C_Mem_Write(&hi2c1, SD3078_I2C_ADDR_WR,
                           SD3078_REG_SEC, I2C_MEMADD_SIZE_8BIT,
                           buf, 7, 100);

    // 无论成功失败都重新上锁
    sd3078_lock_write();
    return st;
}

bool rtc_sd3078_is_running(void)
{
    uint8_t sec;
    if (sd3078_read_reg(SD3078_REG_SEC, &sec) != HAL_OK)
        return false;
    // bit7 (ST 标志) = 1 表示停止
    return (sec & 0x80) == 0;
}

/* ---------- Unix 时间转换（纯数学，不碰硬件） ---------- */

uint32_t rtc_sd3078_to_unix(const rtc_datetime_t *dt)
{
    int days = 0;
    int y;

    // 从 2000-01-01 算起的天数
    for (y = 2000; y < dt->year; y++) {
        days += ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 366 : 365;
    }
    // 本年累计天数。闰年 2 月要按 29 天算，否则 3 月起会少算 1 天
    const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int leap_day = ((dt->year % 4 == 0 && dt->year % 100 != 0) ||
                    (dt->year % 400 == 0)) ? 1 : 0;
    for (int m = 0; m < dt->month - 1; m++)
        days += mdays[m] + ((m == 1) ? leap_day : 0);
    days += (dt->day - 1);

    return (uint32_t)days * 86400U
         + (uint32_t)dt->hour * 3600U
         + (uint32_t)dt->minute * 60U
         + (uint32_t)dt->second;
}

void rtc_sd3078_from_unix(uint32_t unix_s, rtc_datetime_t *dt)
{
    if (dt == NULL) return;

    const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t days = unix_s / 86400U;
    uint32_t rem  = unix_s % 86400U;

    dt->hour    = (uint8_t)(rem / 3600U);
    rem        %= 3600U;
    dt->minute  = (uint8_t)(rem / 60U);
    dt->second  = (uint8_t)(rem % 60U);

    int y = 2000;
    while (1) {
        int leap = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 366 : 365;
        if (days < (uint32_t)leap) break;
        days -= leap;
        y++;
    }
    dt->year = (uint16_t)y;

    int leap_day = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 1 : 0;
    int m = 0;
    while (m < 11 && days >= (uint32_t)(mdays[m] + (m == 1 ? leap_day : 0))) {
        days -= (uint32_t)(mdays[m] + (m == 1 ? leap_day : 0));
        m++;
    }
    dt->month = (uint8_t)(m + 1);
    dt->day   = (uint8_t)(days + 1);
    dt->weekday = 0; // 暂不计算星期
}
