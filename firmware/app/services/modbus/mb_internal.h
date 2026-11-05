/**
 * @file mb_internal.h
 * @brief Modbus 内部共享声明（仅 .c 文件 include，不对外暴露）
 *
 *  定义跨文件共享的全局变量 extern 声明和内部辅助函数。
 *  外部模块只 include mb_api.h，不要碰这个文件。
 */
#ifndef __MB_INTERNAL_H__
#define __MB_INTERNAL_H__

#include "mb_api.h"
#include "modbus.h"

/* ========= 跨文件共享全局（定义在 mb_poll.c）========= */
extern mb_handle_t *mb_handle;
extern modbusHandler_t hModbusMaster;

/* ========= 内部函数（定义在 mb_slave.c）========= */
/**
 * @brief 触发配置变更回调（内部统一入口，检查 cb_suppress 抑制标志）
 */
void mb_fire_cfg_change(mb_cfg_change_reason_t reason, uint8_t slave_id);

/* ========= 不持 cfg 锁的内部版 CRUD（供组合函数在已持锁状态下调用）========= */
/**
 * @brief 添加从站核心逻辑（不加锁，调用方必须已持 xSlaveCfgMutex）
 *
 * @param[in,out] existed  输出：调用前该 slave_id 是否已存在（供外部决定 ADD/UPDATE 回调类型）
 */
mb_status_e mb_add_slave_nolock(modbus_t *cfg, uint8_t *existed);

/**
 * @brief 删除从站核心逻辑（不加锁，调用方必须已持 xSlaveCfgMutex）
 */
mb_status_e mb_delete_slave_nolock(uint8_t slave_id);

/**
 * @brief 扩容从站列表核心逻辑（不加锁，调用方必须已持 xSlaveCfgMutex）
 */
mb_status_e mb_realloc_slave_list_nolock(void);

/* ========= 不持 cfg 锁的内部版点位表写入（同上）========= */
int mb_pointmap_set_slave_points_nolock(uint8_t slave_id, const mb_point_t *points);

#endif /* __MB_INTERNAL_H__ */
