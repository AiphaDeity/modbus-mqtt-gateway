/*
 * mqtt_client.h —— M2 上云模块 MQTT 3.1.1 客户端（重写版）
 *
 * M2 职责范围（依据《任务分工》与《系统设计说明书》）：
 *   - MQTT 3.1.1 协议栈：CONNECT/SUBSCRIBE/PUBLISH/PINGREQ/CONNACK/SUBACK/PINGRESP
 *   - TCP 连接 broker（LwIP netconn API），心跳保活，断连自动重连
 *   - 下行命令分发：订阅 cmd 主题 -> 解析 JSON -> 调 M1 mb_inject_write 写寄存器 -> 回 ACK
 *   - 离线补传驱动：MQTT 连上后从 M3 storage_m2 队列拉历史记录逐条 PUBLISH + 发 ack 回执
 *     （旧版独立的 replay_task 已删除，补传统一走 storage_m2 协议，避免两套缓存机制冲突）
 *   - 遥测由telemetry_task采集入库；MQTT统一发送已提交记录并补record_id，PUBACK不回收缓存。
 *
 * 对外接口（5 个，被 app_modules.c 装配表 + telemetry_task.c 调用，签名不可变）：
 *   MqttClient_Init / MqttClient_Task / MqttClient_Publish / MqttClient_IsConnected / MqttClient_ReportEvent
 *
 * 设计取舍（RAM 128KB 已用 ~86%）：
 *   - 收发缓冲 static，放 CCMRAM，不占任务栈
 *   - PUBLISH 支持 QoS0 和 QoS1（设计 5.2: OTA状态/补传用QoS1可靠传输，遥测用QoS0）
 *   - QoS1: 带 packet_id + 等 PUBACK + 30s 超时重发（设计 5.2），pending 队列放 CCMRAM
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

/* 仅任务上下文设置暂停请求；每次请求均撤销旧确认，MQTT保活继续运行。 */
void MqttClient_SetPaused(bool paused);
/* 任务到达安全点、旧补传应答已结算且发送锁空闲时返回true。 */
bool MqttClient_IsPaused(void);

/**
 * @brief  线程安全的 PUBLISH（QoS0）——telemetry_task 等其他任务可并发调用
 * @param  topic   主题字符串（\0 结尾）
 * @param  payload 数据指针
 * @param  plen    数据长度
 * @return 0=成功，-1=未连接或发送锁不可用，-2=发送失败，-3=payload过大，-5=业务暂停
 * @note   锁等待最多12秒、写入最多10秒（另计调度延迟），写入失败关闭发送准入。
 *         MQTT任务自行报告健康，其他调用任务须覆盖本次阻塞等待。
 */
int MqttClient_Publish(const char *topic, const uint8_t *payload, size_t plen);

/**
 * @brief  线程安全的 PUBLISH（QoS1）——OTA状态/补传等需要可靠传输的场景
 * @param  topic   主题字符串（\0 结尾）
 * @param  payload 数据指针
 * @param  plen    数据长度（≤RECORD_PAYLOAD_MAX，与存储记录上限一致）
 * @return 0=已发送(等PUBACK), -1=未连接或发送锁不可用, -2=发送失败, -3=过大, -4=pending队列满, -5=业务暂停
 * @note   内部拷贝 payload 到 pending 槽，函数返回后 payload 可释放。
 *         PUBACK 30s 未到则保持原 packet_id 重发（设计 5.2）。
 *         锁/写入等待和健康报告责任同QoS0接口。
 */
int MqttClient_PublishQos1(const char *topic, const uint8_t *payload, size_t plen);
/*
 * 仅网络升级任务提交本次启动的同一份已核验结果，payload在返回前有效且不变。
 * 复用ota_status和原QoS1槽；0=已发送/仍等PUBACK，1=匹配PUBACK已到，负值同QoS1。
 * 断线未确认时须重调；PUBACK仅证明broker接收，不表示云端任务已处理。
 */
int MqttClient_PublishUpgradeResult(const uint8_t *payload, size_t plen);

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

/**
 * @brief  获取遥测上报主题（Init 时按当前上云模式拼好）
 * @note   telemetry_task 用它替代硬编码 topic，切云平台时自动适配
 * @return 主题字符串指针（只读，生命周期同 mqtt 模块）
 */
const char *MqttClient_TopicTelemetry(void);

// 复制本次运行使用的网关身份；优先已置备身份，否则取配置ClientID；容量含NUL，失败false。
bool MqttClient_GetGatewayId(char *out, size_t capacity);

/**
 * @brief  获取 OTA 状态上报主题（Init 时按当前上云模式拼好）
 * @note   network_upgrade 模块用它上报升级状态到云端
 * @return 主题字符串指针（只读，生命周期同 mqtt 模块）
 */
const char *MqttClient_TopicOtaStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_H */
