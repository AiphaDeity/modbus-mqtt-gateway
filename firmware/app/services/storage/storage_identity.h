#ifndef STORAGE_IDENTITY_H
#define STORAGE_IDENTITY_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * 设备身份服务（依据设计说明书 7.4 节 / 7.5 节）。
 *
 * 身份区 0x040000-0x080000，双份冗余：
 *   副本 A = 0x040000，副本 B = 0x041000（各占一个 4KiB 扇区）
 *
 * 写入策略：始终写"较旧的那份"，保留另一份作回退，
 * 因此任意时刻掉电都至少有一份完整可读。
 */

#define GATEWAY_ID_MAX_LEN 32

// 初始化：读取双份，选出有效且最新的身份
HAL_StatusTypeDef storage_identity_init(void);

// 是否已存在有效身份
bool storage_identity_has_id(void);

// 读取 gateway_id（拷贝到 out，超出 out_size 会截断并以 '\0' 结尾）
HAL_StatusTypeDef storage_identity_get(char *out, size_t out_size);

// 设置 gateway_id（自动写入较旧副本，generation 递增）
HAL_StatusTypeDef storage_identity_set(const char *gateway_id);

#endif /* STORAGE_IDENTITY_H */
