#ifndef TELEMETRY_TASK_H
#define TELEMETRY_TASK_H

#include "app_modules.h"

AppModuleInitResult Telemetry_Init(void);
void Telemetry_Task(void *argument);

/* 仅任务上下文设置暂停请求；true重新等待本任务确认，不代表已经停稳。 */
void Telemetry_SetPaused(bool paused);
/* 本任务已退出当前采集、入队及发送操作后返回true。 */
bool Telemetry_IsPaused(void);

#endif
