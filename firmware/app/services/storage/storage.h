#ifndef STORAGE_H
#define STORAGE_H

#include "app_modules.h"
#include "cmsis_os2.h"

/*
 * M3 存储与时间服务模块对外接口。
 * 装配层（app_modules）通过这两个函数接入 M3。
 */

// I2C1 总线互斥锁（SD3078、AHT20、INA226、PCA9555 共用）。
// 由 Storage_Init 创建，所有调用 I2C1 的代码必须先获取此锁。
extern osMutexId_t g_m3_i2c1_mutex;

// 基础初始化：准备私有状态、互斥锁，不创建任务。
AppModuleInitResult Storage_Init(void);

// 存储模块主任务：恢复记录、上报就绪位，业务循环中处理读写请求。
void Storage_Task(void *argument);

#endif /* STORAGE_H */
