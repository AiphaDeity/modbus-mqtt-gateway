#ifndef APP_READINESS_H
#define APP_READINESS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 资源状态由对应主责在任务上下文更新，每次只更新一个资源。
 * 初始化和任务创建成功不自动置位；恢复完成后置位，失效时及时清除。
 */
typedef enum
{
  // M1：当前运行配置已读取并验证；保存候选配置不改变这个状态
  APP_RESOURCE_CONFIG = (1U << 0),
  // M3：TF 记录提交服务已就绪；不代表 SPI 配置读写服务，也不代表某条记录已落盘
  APP_RESOURCE_STORAGE = (1U << 1),
  // M2：以太网传输及 IP 可用；不代表 MQTT 已连接或已收到业务回执
  APP_RESOURCE_NETWORK = (1U << 2),
  // M3：已校时且 RTC 时间有效
  APP_RESOURCE_TIME = (1U << 3),
  // M3：控制日志、去重和持久时间下界已恢复
  APP_RESOURCE_CONTROL_STATE = (1U << 4)
} AppResource;

#define APP_RESOURCE_ALL ((uint32_t)(APP_RESOURCE_CONFIG | APP_RESOURCE_STORAGE | \
                                     APP_RESOURCE_NETWORK | APP_RESOURCE_TIME | \
                                     APP_RESOURCE_CONTROL_STATE))

typedef enum
{
  APP_WAIT_READY = 0,
  APP_WAIT_TIMEOUT,
  APP_WAIT_INVALID_ARGUMENT,
  APP_WAIT_ERROR
} AppWaitResult;

/*
 * 由 App_Entry 在模块初始化前创建静态就绪事件组，初始全部未就绪。
 * 返回是否成功；仅用于启动任务，重复调用保留已有状态。
 */
bool App_ReadinessInit(void);

// 提供方报告单个资源的当前状态；非法资源、未初始化或 RTOS 操作失败返回 false
bool App_SetResourceReady(AppResource resource, bool ready);

// 返回当前就绪位快照；未初始化或查询失败时按全部未就绪处理
uint32_t App_GetReadyResources(void);

// 在任务中查询所需资源是否全部就绪；零掩码、未知位返回 false
bool App_IsReady(uint32_t required_resources);

/*
 * 在任务中等待全部指定资源，timeout_ms 单位为毫秒，0 表示立即检查。
 * 等待不清除就绪位，允许多个任务观察；禁止使用 UINT32_MAX 无限等待。
 * 返回只代表一次就绪观察；执行业务前仍须检查当前状态和所属接口的实际结果。
 * 维护、诊断及存储恢复不等待 CONFIG；无配置时不开展采样和 MQTT 业务。
 */
AppWaitResult App_WaitResources(uint32_t required_resources, uint32_t timeout_ms);

#endif
