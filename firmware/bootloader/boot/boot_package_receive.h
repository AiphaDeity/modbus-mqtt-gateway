#ifndef BOOT_PACKAGE_RECEIVE_H
#define BOOT_PACKAGE_RECEIVE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  BOOT_RECEIVE_NO_REQUEST,
  BOOT_RECEIVE_COMMAND,
  BOOT_RECEIVE_PACKAGE
} BootReceiveResult;

/*
 * 只读诊断：初始化 USART1/SPI3 后，重复读取固定地址并打印一致性统计。
 * 不读取串口请求、不擦写 Flash、不修改 SPI 配置；返回后由调用方停留在 Boot。
 * 重复值一致仅表示本轮读回一致，不能独立证明存储内容和硬件全部正确。
 */
void Boot_TestFlashReadDiagnostic(void);

#endif
