#ifndef BOOT_PACKAGE_RECEIVE_H
#define BOOT_PACKAGE_RECEIVE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  BOOT_RECEIVE_NO_REQUEST,
  BOOT_RECEIVE_COMMAND,
  BOOT_RECEIVE_PACKAGE,
  BOOT_RECEIVE_EXIT
} BootReceiveResult;

/*
 * 本地维护/恢复：初始化USART1/SPI2后、跳转App前同步调用。
 * 请求窗口固定10秒，分段等待每次最多250ms，收到字节不延长窗口。
 * 普通收包只写全空槽，独立恢复须显式指定允许擦写的槽。
 * 正常收包完成认证后登记候选/旧包；恢复命令交给独立恢复模块。
 * 无请求返回NO_REQUEST，半截请求/控制命令/失败返回COMMAND，已登记包返回PACKAGE。
 * 本函数不安装App，调用方根据结果运行安装器或继续等待串口。
 * keys可为NULL，此时仍允许查询及显式全空事务号首次配置，拒绝包擦写；不保存指针。
 */
BootReceiveResult Boot_ReceivePackage(const uint8_t *public_key, const uint8_t *aes_key);

/*
 * 在 Boot 主线程按 256 B 停等协议收包，独占已初始化的 USART1/SPI2 和目标包槽。
 * address 为可写槽起点，total_size 为完整包字节数；recovery 选择恢复写入口。
 * 单页接收限 2 s，整包限 180 s（不含此前擦槽和后续认证），HAL 时基须前进。
 * 完整收包、逐页读回及最后 ACK 均在期限内完成才返回 true；失败保留半包，禁止登记。
 * 到期锁存监督失败，由主流程复位；阻塞调用前后检查软件期限不能替代硬件看门狗。
 */
bool Boot_ReceivePackageData(uint32_t address, uint32_t total_size, bool recovery);

/*
 * 只读诊断：初始化 USART1/SPI2 后，重复读取固定地址并打印一致性统计。
 * 不读取串口请求、不擦写 Flash、不修改 SPI 配置；返回后由调用方停留在 Boot。
 * 重复值一致仅表示本轮读回一致，不能独立证明存储内容和硬件全部正确。
 */
void Boot_TestFlashReadDiagnostic(void);

#endif
