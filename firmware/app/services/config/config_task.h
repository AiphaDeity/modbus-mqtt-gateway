#ifndef __CONFIG_TASK_H__
#define __CONFIG_TASK_H__

#include "app_modules.h"

/*
 * 配置模块接口，实现在 config.c。
 * 启动加载和 TF 导入由 Config_Task 经 Storage 请求完成；
 * MQTT 配置变更由 Config_HandleMessage 同步处理。
 */

// 绑定配置模块的升级暂停接口；初始化阶段不读写存储，也不创建任务。
AppModuleInitResult Config_Init(void);

// 加载并发布配置、处理 TF 保存与取消结算，按实际进展报告健康状态。
void Config_Task(void *argument);

/*
 * 同步处理 MQTT 的从站 upsert/delete JSON，调用期间字符串须保持有效并以 NUL 结尾。
 * 仅允许普通任务上下文；调用方负责回执，本接口不保证变更已持久化。
 * 返回 0=成功、-1=解析失败、-2=升级门控拒绝、-3=部分从站应用失败。
 */
int Config_HandleMessage(const char *json_buf);

#endif /* __CONFIG_TASK_H__ */
