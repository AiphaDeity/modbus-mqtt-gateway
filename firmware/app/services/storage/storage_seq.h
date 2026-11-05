#ifndef STORAGE_SEQ_H
#define STORAGE_SEQ_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * 序列号管理（依据设计说明书 7.4 节）。
 *
 * - seq 为 64 位无符号整数
 * - 外部 Flash 持久记录 next_reserved（高水位）
 * - 每次预留 1024 个序号
 * - 先原子提交新高水位，再使用窗口内编号
 * - 重启跳过未用预留号，允许跳号、禁止复用
 * - 接近最大值时停止生成，不回绕
 */

/* 每次预留的序号数量 */
#define SEQ_RESERVE_BATCH   1024U

/* 空设备首发序号（M2 约定：全局单计数器从 1001 起，跨重启连续） */
#define SEQ_FIRST_VALUE     1001U

/* 初始化序列号服务：从 Flash 读取 next_reserved */
HAL_StatusTypeDef storage_seq_init(void);

/* 分配一个新序号（从当前窗口取，窗口用完则预留下一批） */
HAL_StatusTypeDef storage_seq_alloc(uint64_t *seq_out);

/* 获取当前已持久化的高水位 */
uint64_t storage_seq_get_high_watermark(void);

#endif /* STORAGE_SEQ_H */
