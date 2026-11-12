/*
 * https_tls_stubs.c —— mbedTLS 平台依赖桩（TLS 客户端专用）
 *
 * 提供 mbedTLS 在裸机 STM32F4 上运行所需的平台函数：
 *   1. mbedtls_ms_time()          —— 毫秒时间戳（HAL_GetTick）
 *   2. mbedtls_platform_time      —— 秒时间戳（SD3078 RTC → storage_get_unix_time）
 *   3. mbedtls_hardware_poll()    —— 硬件熵源（STM32F407 RNG 外设）
 *   4. _gettimeofday()            —— newlib 时间桩（TLS 不直接调，但 time.h 间接引用）
 *
 * 编译时使用 https_tls_mbedtls_config.h 配置（非 upgrade_mbedtls_config.h）。
 */
#include "stm32f4xx_hal.h"
#include "stm32f407xx.h"          /* RNG 寄存器定义（含 RNG_CR_RNGEN / RNG_SR_DRDY 等） */
#include "storage_time.h"         /* storage_get_unix_time() */
#include "cmsis_os2.h"

/* mbedTLS 头（通过 https_tls_mbedtls_config.h 配置） */
#include "mbedtls/platform_time.h"   /* mbedtls_ms_time_t, mbedtls_platform_set_time */
#include "mbedtls/entropy.h"         /* mbedtls_hardware_poll 声明 */

#include <string.h>
#include <sys/time.h>
#include <errno.h>

/* ===== 1. 毫秒时间戳 =====
 * MBEDTLS_PLATFORM_MS_TIME_ALT 要求 app 提供 mbedtls_ms_time()。
 * HAL_GetTick() 返回 uwTick（毫秒，FreeRTOS SysTick 递增），单调递增。
 */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)HAL_GetTick();
}

/* ===== 2. 秒时间戳 =====
 * MBEDTLS_PLATFORM_TIME_ALT 要求 app 提供 mbedtls_time 函数指针。
 * 通过 mbedtls_platform_set_time() 注册，在 HttpsTls_Init() 中调用。
 *
 * 返回 Unix 时间戳（秒，2000-01-01 起）。
 * time 非空时同时写入 *time（与标准 C time() 语义一致）。
 */
static mbedtls_time_t tls_platform_time(mbedtls_time_t *t)
{
    uint32_t now = storage_get_unix_time();
    if (t != NULL) *t = (mbedtls_time_t)now;
    return (mbedtls_time_t)now;
}

/* 在 TLS 模块初始化时调用，注册时间函数指针 */
void https_tls_register_time(void)
{
    mbedtls_platform_set_time(tls_platform_time);
}

/* ===== 3. 硬件熵源 =====
 * MBEDTLS_ENTROPY_HARDWARE_ALT + MBEDTLS_NO_PLATFORM_ENTROPY 要求
 * app 提供 mbedtls_hardware_poll()。
 *
 * STM32F407 RNG 外设：
 *   - AHB2 总线，时钟由 RCC_AHB2ENR.RNGEN 使能
 *   - CR.RNGEN 启动 RNG
 *   - SR.DRDY=1 时 DR 有 32bit 随机数
 *   - SR.CECS/SECS=1 时时钟/种子错误（返回失败）
 */
/* STM32F407 RNG 外设寄存器宏（RNG_CR_RNGEN / RNG_SR_DRDY / RNG_SR_CECS / RNG_SR_SECS）
 * 已由 stm32f407xx.h CMSIS 头定义，此处不重复定义。
 *
 * RCC 时钟使能通过 __HAL_RCC_RNG_CLK_ENABLE() 宏（stm32f4xx_hal_rcc_ex.h）。
 */

static bool rng_ready = false;

static void rng_init_once(void)
{
    if (rng_ready) return;
    /* 使能 RNG 时钟（AHB2） */
    __HAL_RCC_RNG_CLK_ENABLE();   /* RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN */
    /* 启动 RNG */
    RNG->CR = RNG_CR_RNGEN;
    rng_ready = true;
}

int mbedtls_hardware_poll(void *data,
                           unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    rng_init_once();

    size_t filled = 0;
    while (filled < len)
    {
        /* 等待 DRRDY（最多轮询 100 次，约 100µs） */
        uint32_t timeout = 100;
        while (!(RNG->SR & RNG_SR_DRDY))
        {
            if (--timeout == 0)
            {
                *olen = filled;
                return -1;   /* 超时 */
            }
        }

        /* 检查错误标志 */
        if (RNG->SR & (RNG_SR_CECS | RNG_SR_SECS))
        {
            *olen = filled;
            return -2;   /* 时钟/种子错误 */
        }

        /* 读 32-bit 随机数据 */
        uint32_t val = RNG->DR;
        size_t copy = len - filled;
        if (copy > 4) copy = 4;
        memcpy(output + filled, &val, copy);
        filled += copy;
    }

    *olen = filled;
    return 0;
}

/* ===== 4. newlib _gettimeofday 桩 =====
 * newlib time.h 某些路径间接引用 _gettimeofday（如 gmtime 内部）。
 * 提供桩避免链接缺符号；返回 RTC 秒时间戳。
 */
int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv)
    {
        tv->tv_sec = (long)storage_get_unix_time();
        tv->tv_usec = 0;
    }
    return 0;
}
