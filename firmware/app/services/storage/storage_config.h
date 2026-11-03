#ifndef STORAGE_CONFIG_H
#define STORAGE_CONFIG_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * 配置存储服务 —— A/B/C 三槽事务（依据设计说明书 6.2 节）。
 *
 * 三槽：
 *   A = 0x000000, B = 0x014000, C = 0x028000（每槽 80KiB）
 *   R = 当前运行槽（generation 最新且完整）
 *   S = 最近成功保存槽
 *   C = 候选槽（既非 R 也非 S）
 *
 * 事务流程：
 *   BEGIN（擦候选）→ CHUNK（分块写 JSON）→ VALIDATE（校验 CRC）
 *   → COMMIT（写提交标记，候选变新 S）
 *
 * M3 只负责持久化 JSON 字节流，不解析 JSON 内容。
 * JSON 解析与字段校验由配置维护模块负责。
 */

// 启动时加载当前有效配置的 JSON 数据
// json_buf: 调用方提供的缓冲区，buf_size 应 >= 64KiB
// actual_len: 返回实际 JSON 长度
HAL_StatusTypeDef storage_config_load(uint8_t *json_buf, uint32_t buf_size, uint32_t *actual_len);

// 开始配置写入事务：擦除候选槽，返回 session_id
HAL_StatusTypeDef storage_config_begin(uint32_t total_len, uint32_t *session_id);

// 分块写入配置数据（offset 必须从 0 连续递增）
HAL_StatusTypeDef storage_config_chunk(uint32_t session_id, uint32_t offset,
                                        const uint8_t *data, uint32_t len);

// 校验已写入数据的完整性（CRC32）
HAL_StatusTypeDef storage_config_validate(uint32_t session_id);

// 提交配置：写提交标记，候选槽成为新的保存槽 S
HAL_StatusTypeDef storage_config_commit(uint32_t session_id);

// 取消当前事务（不影响 R 和旧 S）
void storage_config_abort(uint32_t session_id);

// 获取当前运行配置的 generation
uint32_t storage_config_get_generation(void);

// 是否存在有效配置
bool storage_config_has_valid(void);

#endif /* STORAGE_CONFIG_H */
