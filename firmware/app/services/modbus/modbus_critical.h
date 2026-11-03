#ifndef MODBUS_CRITICAL_H
#define MODBUS_CRITICAL_H

#if defined(USE_FREERTOS)
#include "FreeRTOS.h"
#include "task.h"
#define MB_CRITICAL_ENTER()    taskENTER_CRITICAL()
#define MB_CRITICAL_EXIT()     taskEXIT_CRITICAL()

#elif defined(USE_RTTHREAD)
#include "rtthread.h"
#define MB_CRITICAL_ENTER()    rt_enter_critical()
#define MB_CRITICAL_EXIT()     rt_exit_critical()

#elif defined(USE_BARE_METAL)
#include "core_cm4.h"  // 内核不对自行改成 core_cm3.h / core_cm7.h
#define MB_CRITICAL_ENTER()    uint32_t primask_val = __get_PRIMASK(); __set_PRIMASK(1)
#define MB_CRITICAL_EXIT()     __set_PRIMASK(primask_val)

#else
#error "必须定义 USE_FREERTOS / USE_RTTHREAD / USE_BARE_METAL 其中一个！"
#endif

#endif