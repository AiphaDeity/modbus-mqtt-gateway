/*
 * mqtt_client.h —— M2 上云模块最小 MQTT 3.1.1 客户端骨架
 *
 * 目标：先用 LwIP netconn API 手写一个最小客户端，跑通
 *   TCP connect -> MQTT CONNECT -> CONNACK -> PUBLISH status
 * 不引 paho 等重型库（RAM 不够），协议只实现当前必须的子集。
 *
 * 当前阶段：连接本地 broker（PC 上跑的 mosquitto）验证链路。
 * 之后再补：订阅命令下行、断网重连、遗嘱报文、QoS。
 */
#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "app_modules.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 由 app_modules.c 在装配表里调用：返回 OK 即表示模块可启动 */
AppModuleInitResult MqttClient_Init(void);

/* 任务入口：netconn connect -> CONNECT -> CONNACK -> PUBLISH status -> 挂起等待 */
void MqttClient_Task(void *argument);

/* 线程安全的 PUBLISH（QoS0）：其他任务（telemetry/replay）可调用。
 * 未连接或写失败返回非 0。topic/payload 都在调用方缓冲里。 */
int MqttClient_Publish(const char *topic, const uint8_t *payload, size_t plen);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_H */
