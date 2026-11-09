/*
 * mqtt_client.h —— M2 上云模块 MQTT 3.1.1 客户端（重写版）
 *
 * M2 职责范围（依据《任务分工》与《系统设计说明书》）：
 *   - MQTT 3.1.1 协议栈：CONNECT/SUBSCRIBE/PUBLISH/PINGREQ/CONNACK/SUBACK/PINGRESP
 *   - TCP 连接 broker（LwIP netconn API），心跳保活，断连自动重连
 *   - 下行命令分发：订阅 cmd 主题 -> 解析 JSON -> 调 M1 mb_inject_write 写寄存器 -> 回 ACK
 *   - 离线补传驱动：MQTT 连上后从 M3 storage_m2 队列拉历史记录逐条 PUBLISH + 发 ack 回执
 *     （旧版独立的 replay_task 已删除，补传统一走 storage_m2 协议，避免两套缓存机制冲突）
 *   - 遥测上报由 telemetry_task 负责（周期采 Modbus -> JSON -> M3 缓存 + 在线直发）
 *
 * 对外接口（5 个，被 app_modules.c 装配表 + telemetry_task.c 调用，签名不可变）：
 *   MqttClient_Init / MqttClient_Task / MqttClient_Publish / MqttClient_IsConnected / MqttClient_ReportEvent
 *
 * 设计取舍（RAM 128KB 已用 ~86%）：
 *   - 收发缓冲 static，放 CCMRAM，不占任务栈
 *   - 只做 QoS0（CONNECT/SUBSCRIBE 用 QoS1 但不重传，靠 broker 兜底）
 *   - 不引入 cJSON，下行命令用手写最小 JSON 解析
 *   - Publish 加互斥锁，多任务并发安全
 *
 * 协议参考：OASIS MQTT v3.1.1 (MQTT-3.1.1)
 */
#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "app_modules.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 对外接口（签名与旧版一致，调用方无需改动） ===== */

/**
 * @brief  MQTT 客户端初始化（由 app_modules.c 装配表调用）
 * @note   只初始化状态量、互斥锁、加载配置；不连网络
 * @return APP_MODULE_INIT_OK=成功，APP_MODULE_INIT_FAILED=互斥锁创建失败
 */
AppModuleInitResult MqttClient_Init(void);

/**
 * @brief  MQTT 客户端任务入口（FreeRTOS 任务，由装配表创建）
 * @note   死循环：等网络就绪 -> 连 broker -> 订阅 -> 收消息/心跳/补传 -> 断连重连
 */
void MqttClient_Task(void *argument);

/**
 * @brief  线程安全的 PUBLISH（QoS0）——telemetry_task 等其他任务可并发调用
 * @param  topic   主题字符串（\0 结尾）
 * @param  payload 数据指针
 * @param  plen    数据长度
 * @return 0=成功，-1=未连接，-2=发送失败，-3=payload 过大
 * @note   内部加互斥锁保护 tx_buf 和 netconn_write
 */
int MqttClient_Publish(const char *topic, const uint8_t *payload, size_t plen);

/**
 * @brief  查询 MQTT 是否已连接到 broker
 * @return true=已连接，false=未连接/断连中
 */
bool MqttClient_IsConnected(void);

/**
 * @brief  上报事件（上线/离线/设备异常等）到 event 主题
 * @param  event_type 事件类型字符串（如 "online"/"offline"/"alarm"）
 * @param  detail    事件详情字符串
 * @return 0=成功，非0=失败
 */
int MqttClient_ReportEvent(const char *event_type, const char *detail);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_H */
