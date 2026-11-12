/**
 * @file mb_fixed_points.h
 * @brief 编译期固定只读点表 —— 基于实际设备寄存器手册制作
 *
 *  设备清单（4 站 × 各 1 点，远低于 16 点/站上限）：
 *
 *  站1: B-RS-L30 光照传感器（淮安蓝控）
 *    - Modbus 地址 1（出厂默认）
 *    - FC03 读保持寄存器，起始 0x0002，数量 2（32 位光照值）
 *    - 换算: raw(uint32) × 0.001 = Lux
 *    - 来源: 光照传感器(1).pdf 第 2 章 2.1 节
 *    - 示例: 0x000CF473 = 849011 → 849.01 Lux
 *
 *  站2~4: TAS-SJ-R 水浸传感器（杭州塔石，王字壳）
 *    - Modbus 地址 2/3/4（现场配置）
 *    - FC03 读保持寄存器，起始 0x2380，数量 1（水浸状态）
 *    - 换算: raw(uint16) × 1.0，0=无水 1=有水
 *    - 来源: 塔石传感器寄存器定义说明_V1.6 第 1.30 节
 *    - 注: 灵敏度(0x0038)和延时(0x2400)是配置值不在周期采集范围
 *
 *  串口公共参数: 9600,8,N,1（两种设备出厂默认一致）
 */
#ifndef MB_FIXED_POINTS_H
#define MB_FIXED_POINTS_H

#include <stdint.h>
#include <stdbool.h>
#include "mb_api.h"

/* ===== 固定点表配置 ===== */
#define MB_FIXED_SLAVE_COUNT   4U   /* 实际从站数量 */

/* 站地址 */
#define MB_FIXED_ADDR_LIGHT    1U   /* 光照传感器 */
#define MB_FIXED_ADDR_LEAK_1   2U   /* 水浸传感器 #1 */
#define MB_FIXED_ADDR_LEAK_2   3U   /* 水浸传感器 #2 */
#define MB_FIXED_ADDR_LEAK_3   4U   /* 水浸传感器 #3 */

/* 串口参数（两种设备出厂默认一致） */
#define MB_FIXED_BAUDRATE      9600U
#define MB_FIXED_DATA_BITS     8U
#define MB_FIXED_STOP_BITS     1U
#define MB_FIXED_PARITY        0U   /* 0=NONE */

/* 采集周期 */
#define MB_FIXED_POLL_MS_LIGHT   2000U   /* 光照 2 秒 */
#define MB_FIXED_POLL_MS_LEAK    1000U   /* 水浸 1 秒（报警及时性） */

/* 寄存器地址 */
#define MB_FIXED_REG_LIGHT       0x0002U   /* 光照值高 16 位（32 位跨 2 寄存器） */
#define MB_FIXED_REG_LIGHT_CNT   2U
#define MB_FIXED_REG_LEAK        0x2380U   /* 水浸状态 */
#define MB_FIXED_REG_LEAK_CNT    1U

/**
 * @brief 启动时应用编译期固定点表
 *
 *  在 Modbus_Init 之后、Poll 循环开始前调用。
 *  使用 mb_config_batch_begin/end 批量添加，只触发一次 RELOAD 回调。
 *  幂等：重复调用会先清空已有从站再添加。
 *
 *  @return true 成功添加全部从站；false 失败
 */
bool mb_fixed_points_apply(void);

#endif /* MB_FIXED_POINTS_H */
