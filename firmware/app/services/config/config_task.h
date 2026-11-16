/**
 * @file config_task.h
 * @brief 配置维护模块 —— 运行时从站配置变更入口
 *
 *  职责：
 *    1. 上电时加载从站配置（三级降级）：
 *       TF 卡 config.json → Flash 存储 → 编译期固定点表
 *    2. 运行时处理 MQTT 下发的配置变更命令：
 *       解析 JSON → 调 mb_apply_slave_config → 触发持久化回调
 *
 *  配置 JSON Schema（云端下发 / TF 卡 config.json）：
 *    {
 *      "op": "upsert",                    // 操作：upsert(新增/更新) 或 delete(删除)
 *      "slaves": [                         // 从站数组
 *        {
 *          "id": 1,                        // Modbus 从站地址 (1~247)
 *          "fc": 3,                        // 功能码 (1~4)
 *          "reg": 2,                       // 起始寄存器地址
 *          "count": 2,                     // 寄存器数量
 *          "interval": 2000,               // 采集周期(ms)
 *          "enable": true,                 // 是否启用
 *          "retry": 1,                     // 重试次数
 *          "points": [                     // 点位表
 *            {
 *              "reg_index": 0,             // 在 reg_data_buf 中的起始下标
 *              "type": "uint32",           // 数据类型：uint16/int16/uint32/int32/float32/bit
 *              "scale": 0.001,             // 系数：raw * scale
 *              "offset": 0,                // 偏移：+ offset
 *              "name": "light",            // 点位名称（≤7字符）
 *              "unit": "Lux",              // 单位（≤7字符）
 *              "bit_index": 0              // 仅 type=bit 时用
 *            }
 *          ]
 *        }
 *      ]
 *    }
 *
 *  架构位置：
 *    APP_MODULE_CONFIG —— 在 Storage 之后、Modbus 之前初始化
 *    Config_Init 上电加载配置，Config_Task 运行时等变更命令
 */
#ifndef __CONFIG_TASK_H__
#define __CONFIG_TASK_H__

#include "app_modules.h"           // AppModuleInitResult 定义在这里

/* ===== 对外接口 ===== */

/**
 * @brief 配置模块初始化（装配层调用）
 *
 *  上电时加载从站配置，三级降级：
 *    1. TF 卡有 config.json → 读文件 → 解析 → 注册从站
 *    2. Flash 有有效配置 → 读 Flash → 解析 → 注册从站
 *    3. 都没有 → 调 mb_fixed_points_apply 用编译期固定点表
 *
 *  @return APP_MODULE_INIT_OK 成功；APP_MODULE_INIT_FAILED 失败
 */
AppModuleInitResult Config_Init(void);

/**
 * @brief 配置维护任务（FreeRTOS 任务入口）
 *
 *  运行时等待 MQTT 下发的配置变更命令：
 *    - MqttClient 收到 commands topic 的配置类消息后调 Config_HandleMessage
 *    - 本任务解析 JSON → 调 mb_apply_slave_config → 回执
 *
 *  @param argument FreeRTOS 任务参数（未使用）
 */
void Config_Task(void *argument);

/**
 * @brief 处理 MQTT 下发的配置变更消息
 *
 *  MqttClient 的 mqtt_handle_command 收到配置类 JSON 后调用此函数。
 *  本函数解析 JSON 并调 mb_apply_slave_config 注册/删除从站。
 *
 *  @param json_buf 配置变更 JSON 字符串（NULL 结尾）
 *  @return 0=成功；-1=解析失败；-2=升级静默期拒绝；-3=应用失败
 */
int Config_HandleMessage(const char *json_buf);

#endif /* __CONFIG_TASK_H__ */
