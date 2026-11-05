/**
 * @file mb_pointmap_flash.h
 * @brief 点位表 / 从站配置持久化 —— 对外接口（不含实现）
 *
 *  本模块只声明"存/取"的契约，具体存到哪里（Flash / EEPROM / 文件系统）
 *  由存储方自行实现。建议实现位置：独立的存储服务模块。
 *
 *  持久化范围（整体快照，建议一次性序列化）：
 *    - 所有从站的 telegram 关键字段（u8id / u8fct / u16RegAdd / u16CoilsNo
 *      / collect_interval_ms / enable / retry_cnt）
 *    - 每个从站的点位表 points[MB_POINTS_PER_SLAVE]
 *    - 全局上报模式（mb_pointmap_get_report_mode）
 *
 *  契约要求：
 *    1. save() 必须做整体写，不能部分写；写完要能以 CRC 校验完整性
 *    2. load() 校验失败必须回退空配置，不能留下半截脏数据
 *    3. 两者都在任务上下文调用，禁止在中断里调用
 *    4. 建议持有 xSlaveCfgMutex 期间只做 RAM 快照拷贝，
 *       真正 I/O 放到锁外，避免阻塞 poll 任务
 */

#ifndef __MB_POINTMAP_FLASH_H__
#define __MB_POINTMAP_FLASH_H__

#include <stdint.h>
#include "mb_pointmap.h"
#include "mb_api.h"

/* ========= 对外 API ========= */

/**
 * @brief 把当前 RAM 配置整体持久化
 *
 *  由存储方在 mb_set_config_change_cb 注册的回调里调用（配置变更时）。
 *
 * @return MB_STATUS_OK 成功；其它 = 存储方自定义错误码
 */
int mb_pointmap_flash_save(void);

/**
 * @brief 从持久化介质整体加载到 RAM
 *
 *  上电初始化阶段调用（mb_handle_init 之后、ModbusStart 之前）。
 *  加载成功后应调用 mb_notify_reload_complete() 通知观察者。
 *
 * @return MB_STATUS_OK 加载成功；
 *         MB_INIT_STATUS_ERR 无有效数据 / 校验失败（调用方走默认配置）
 */
int mb_pointmap_flash_load(void);

#endif /* __MB_POINTMAP_FLASH_H__ */
