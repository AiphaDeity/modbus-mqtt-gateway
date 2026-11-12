#include <reent.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "main.h"

extern uint8_t _siccmram[];
extern uint8_t _sccmram[];
extern uint8_t _eccmram[];
extern uint8_t _sccmram_bss[];
extern uint8_t _eccmram_bss[];

// 在构造函数和 main 前建立 CCM 初值；仅由 preinit 在每次启动时调用一次。
static void AppMemory_InitCcm(void)
{
  __HAL_RCC_CCMDATARAMEN_CLK_ENABLE();
  size_t data_size = (uintptr_t)_eccmram - (uintptr_t)_sccmram;
  size_t bss_size = (uintptr_t)_eccmram_bss - (uintptr_t)_sccmram_bss;
  memcpy(_sccmram, _siccmram, data_size);
  memset(_sccmram_bss, 0, bss_size);
}

// 启动代码的 __libc_init_array 先执行此表，再执行构造函数。
static void (*const ccm_preinit)(void)
  __attribute__((section(".preinit_array"), used)) = AppMemory_InitCcm;

// 串行化任务中的 newlib 分配；调度器启动前无需加锁，ISR 禁止使用 C 堆。
void __malloc_lock(struct _reent *context)
{
  (void)context;
  configASSERT(__get_IPSR() == 0U);
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
    vTaskSuspendAll();
  }
}

// 与分配锁逐层配对，保留调用方原有的调度器挂起状态。
void __malloc_unlock(struct _reent *context)
{
  (void)context;
  configASSERT(__get_IPSR() == 0U);
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
    (void)xTaskResumeAll();
  }
}
