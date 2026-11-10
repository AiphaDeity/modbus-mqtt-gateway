#ifndef TELEMETRY_TASK_H
#define TELEMETRY_TASK_H

#include "app_modules.h"

AppModuleInitResult Telemetry_Init(void);
void Telemetry_Task(void *argument);

#endif
