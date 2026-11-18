#ifndef STORAGE_RUNTIME_H
#define STORAGE_RUNTIME_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>
#include "storage_config.h"

/* 静态资源初始化及Storage_Task生命周期；不创建业务任务。 */
bool storage_runtime_init(void);
bool storage_runtime_start(void);
void storage_runtime_finish(bool ready, bool healthy);
// 存储所有者设置本阶段总期限，timeout_ms 单位 ms；监督另留返回裕量，失败时停止当前操作。
bool storage_runtime_wait(uint32_t timeout_ms);
// 一轮实际工作完成后报告进展和当前自检，结束已声明等待；不直接刷新 IWDG。
void storage_runtime_report(void);
// 所有者在实际 I/O 完成后报告进展并声明下一段有界等待，不改变 I/O 总期限。
void storage_runtime_checkpoint(void);
void storage_runtime_process(void);

/* 业务入口持锁至完整操作结束；试运行、暂停或非存储所有者返回false。 */
bool storage_mutation_begin(void);
void storage_mutation_end(void);
bool storage_mutation_allowed(void);
bool storage_runtime_is_owner(void);

/* 同步调用方通过值拷贝请求分配序号，不在网络任务直接访问Flash。 */
HAL_StatusTypeDef storage_runtime_alloc_seq(uint64_t *seq);

/*
 * 仅客户端任务同步调用，timeout_ms为1～20000 ms，包含排队及I/O。
 * 复用原锁/单槽队列并传值，超时不保留message指针；BEGIN投递前写回session_id供结算。
 */
HAL_StatusTypeDef storage_runtime_config(storage_config_message_t *message, uint32_t timeout_ms);

/* 业务Flash I/O沿用当前操作总期限，地址/长度单位B，只在Storage_Task执行。 */
HAL_StatusTypeDef storage_flash_read(uint32_t addr, uint8_t *data, uint32_t size);
HAL_StatusTypeDef storage_flash_write(uint32_t addr, const uint8_t *data, uint32_t size);
HAL_StatusTypeDef storage_flash_erase(uint32_t addr);
HAL_StatusTypeDef storage_flash_erase64(uint32_t addr);
bool storage_runtime_expired(void);

#endif /* STORAGE_RUNTIME_H */
