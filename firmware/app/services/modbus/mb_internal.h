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

/* 升级接入仅由本模块调用；配置准入必须成对归还。 */
bool mb_runtime_init(void);
bool mb_runtime_poll_begin(void);
void mb_runtime_poll_end(void);
bool mb_runtime_paused(void);
bool mb_runtime_config_begin(void);
void mb_runtime_config_end(void);
bool mb_runtime_enqueue(modbusHandler_t *handler, const modbus_t *telegram, bool front);
bool mb_runtime_take_query(modbusHandler_t *handler, modbus_t *telegram);
void mb_runtime_query_complete(void);
HAL_StatusTypeDef mb_runtime_transmit(modbusHandler_t *handler);
bool mb_runtime_start_health(void);
void mb_runtime_report(void);
void mb_runtime_fail(void);
bool mb_runtime_can_wait(void);
bool mb_write_pending(const mb_write_request_t *request);
void mb_finish_write(const mb_write_request_t *request, mb_errot_t error);
bool mb_write_results_settled(void);

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

/* ========= 不持 cfg 锁的内部版点位表写入（同上）========= */
int mb_pointmap_set_slave_points_nolock(uint8_t slave_id, const mb_point_t *points);

#endif /* __MB_INTERNAL_H__ */
