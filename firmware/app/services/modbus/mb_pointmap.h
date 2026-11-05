/**
 * @file mb_pointmap.h
 * @brief Modbus 点位表配置 —— 从寄存器原始值换算物理量
 *
 *  每个从站可以配置多个点位，每个点位定义：
 *    - reg_index   : 在该从站 reg_data_buf[] 中的起始下标
 *    - data_type   : uint16 / int16 / uint32 / int32 / float32
 *    - scale       : 系数（乘以）
 *    - offset      : 偏移（加上）
 *    - name        : 点位名称（JSON key）
 *    - unit        : 单位
 *
 *  统一换算公式：physical = raw * scale + offset
 *
 *  数据类型占用寄存器数：
 *    uint16 / int16  → 1 个 reg
 *    uint32 / int32 / float32 → 2 个 reg（大端序拼接）
 *
 *  上报模式全局可切换：
 *    RAW     : 只发 data:[123,456] 原始数组（MQTT 方自己解析）
 *    PARSED  : 只发 {"temp":25.1,"hum":60} 结构化 JSON（网关自动换算）
 *    BOTH    : 两种都发
 *
 *  所有配置掉电保存在 Flash，不需要重新编译固件。
 */
#ifndef __MB_POINTMAP_H__
#define __MB_POINTMAP_H__

#include <stdint.h>
#include <stdbool.h>

/* ========= 常量 ========= */
#define MB_POINT_NAME_MAX   16   // 点位名称最大长度（含 '\0'）
#define MB_POINT_UNIT_MAX    8   // 单位最大长度
#define MB_POINTS_PER_SLAVE 32   // 每个从站最多 32 个点位
                                 // （等于 MB_REG_MAX_PER_SLAVE，最多覆盖所有寄存器）

/* ========= 数据类型枚举 ========= */
/**
 * @brief 寄存器解析类型
 *
 *  MODBUS_RTU 标准大端序：高字节在前。
 *  多寄存器拼接时按 reg_index 顺序取连续寄存器。
 */
typedef enum
{
    MB_DT_UINT16  = 0,   // 无符号 16 位 → 1 个 reg
    MB_DT_INT16   = 1,   // 有符号 16 位（补码） → 1 个 reg
    MB_DT_UINT32  = 2,   // 无符号 32 位（reg[0]<<16 | reg[1]） → 2 个 reg
    MB_DT_INT32   = 3,   // 有符号 32 位 → 2 个 reg
    MB_DT_FLOAT32 = 4,   // IEEE754 float → 2 个 reg
    MB_DT_MAX
} mb_data_type_t;

/**
 * @brief 数据类型对应的寄存器占用数（查表用）
 */
static const uint8_t mb_dt_reg_count[MB_DT_MAX] = { 1, 1, 2, 2, 2 };

/* ========= 上报模式 ========= */
typedef enum
{
    MB_REPORT_RAW    = 0,   // 只上报原始寄存器数组
    MB_REPORT_PARSED = 1,   // 只上报解析后的结构化数据
    MB_REPORT_BOTH   = 2,   // 两种都上报
} mb_report_mode_t;

/* ========= 单个点位配置 ========= */
typedef struct
{
    uint16_t reg_index;                          // 在从站 reg_data_buf[] 中的起始下标
    mb_data_type_t data_type;                    // 数据类型
    float    scale;                              // 系数（raw * scale）
    float    offset;                             // 偏移（+ offset）
    char     name[MB_POINT_NAME_MAX];            // 点位名称
    char     unit[MB_POINT_UNIT_MAX];            // 单位
    uint8_t  valid;                              // 0=空位，1=有效配置
} mb_point_t;

/* ========= 解析结果（供 MQTT 方读取） ========= */
typedef struct
{
    char     name[MB_POINT_NAME_MAX];            // 点位名称
    char     unit[MB_POINT_UNIT_MAX];            // 单位
    float    value;                              // 换算后的物理值
    mb_data_type_t raw_type;                     // 原始类型（方便 MQTT 方决定精度）
    uint8_t  valid;                              // 1=解析有效
} mb_parsed_point_t;

/* ========= 对外 API ========= */

/**
 * @brief 设置 / 更新某个从站的点位配置
 *
 *  存储方 / MQTT 下发配置时调用。会触发配置变更回调（如果已注册）。
 *
 * @param slave_id   目标从站（必须已通过 mb_add_slave 注册）
 * @param points     点位数组（长度 = MB_POINTS_PER_SLAVE，空位 valid=0）
 * @return MB_STATUS_OK 成功；MB_SLAVE_NOT_FOUND_ERR 从站未注册
 */
int mb_pointmap_set_slave_points(uint8_t slave_id, const mb_point_t *points);

/**
 * @brief 获取某个从站的点位配置拷贝
 *
 * @param slave_id 目标从站
 * @param out      输出缓冲区（调用方分配 ≥ MB_POINTS_PER_SLAVE 个 mb_point_t）
 * @return 有效点位数量；-1 表示从站不存在
 */
int mb_pointmap_get_slave_points(uint8_t slave_id, mb_point_t *out);

/**
 * @brief 从某个从站的 reg_data_buf 解析所有有效点位
 *
 *  MODBUS poll 任务收到 ERR_OK_QUERY 后自动触发。
 *  MQTT 方也可以主动调用，前提是 data_valid==1。
 *
 * @param slave_id  目标从站
 * @param out       输出数组（调用方分配 ≥ MB_POINTS_PER_SLAVE 个 mb_parsed_point_t）
 * @return 解析成功的点位数量
 */
int mb_pointmap_parse_slave(uint8_t slave_id, mb_parsed_point_t *out);

/**
 * @brief 全局设置上报模式
 */
void mb_pointmap_set_report_mode(mb_report_mode_t mode);

/**
 * @brief 读取当前上报模式
 */
mb_report_mode_t mb_pointmap_get_report_mode(void);

#endif /* __MB_POINTMAP_H__ */
