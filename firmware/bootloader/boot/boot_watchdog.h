#ifndef BOOT_WATCHDOG_H
#define BOOT_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 启动Boot拥有的IWDG；重复调用不重新配置，失败锁存后不得再次启动。
 * 普通上下文调用，HAL时基须正常前进，不修改选项字节。
 */
bool Boot_WatchdogStart(void);

/*
 * 检查固定操作期限，参数单位为ms；started_ms只在操作开始时获取一次。
 * timeout_ms范围1～0x7FFFFFFF，计数回绕用无符号减法处理。
 * 返回false时锁存本次监督失败，直到复位；本函数不喂狗。
 */
bool Boot_WatchdogCheck(uint32_t started_ms, uint32_t timeout_ms);

/*
 * 仅在成功完成一项实际进展后调用，先检查固定期限，再决定是否喂狗。
 * IWDG尚未启动时只检查期限；HAL tick未前进时不会重复刷新IWDG。
 * 返回false表示本次监督已失败，调用方须停止操作。
 */
bool Boot_WatchdogProgress(uint32_t started_ms, uint32_t timeout_ms);

/*
 * 仅在收到请求字节或正常完成一段有界空闲等待后调用。
 * 调用方管理正常等待窗口，不把窗口结束锁存为故障；已有失败仍返回false。
 * IWDG已启动且HAL tick前进时才刷新，普通上下文调用。
 */
bool Boot_WatchdogIdleProgress(void);

// 查询本次启动是否已出现监督失败，不修改或清除状态。
bool Boot_WatchdogFailed(void);

/*
 * 启用DWT周期计数供Boot时序采样，不清零已有计数器；不可用返回false。
 * 调用方以无符号差读取DWT->CYCCNT，HCLK须稳定且间隔小于一次32位回绕。
 * 168MHz下约25.56s回绕；调试暂停不计入正常运行耗时，采样不能代替复位验证。
 */
bool Boot_WatchdogEnableCycleCounter(void);

/*
 * 最终交接前，认证当前持久状态所选包、检查IWDG能力并比对片内App。
 * 普通上下文同步调用，独占SPI3及App镜像，HAL时基须正常前进。
 * 公钥65B、AES密钥32B由可信映射提供，调用期间有效且不变。
 * 无可信记录、状态错误、认证失败或镜像不匹配均返回false，禁止跳转。
 * 调用前main须已启动IWDG；未启狗或监督失败均拒绝，本函数不补启或重新配置。
 */
bool Boot_WatchdogPrepareApp(const uint8_t *public_key, const uint8_t *aes_key);

#endif
