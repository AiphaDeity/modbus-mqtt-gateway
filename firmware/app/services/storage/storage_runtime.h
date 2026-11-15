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
bool storage_runtime_wait(uint32_t timeout_ms);
void storage_runtime_report(void);
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
