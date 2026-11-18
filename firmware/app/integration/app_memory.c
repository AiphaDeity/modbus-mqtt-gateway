#include <reent.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "app_modules.h"

// 以下符号由链接脚本提供：CCM 初值在 Flash，运行地址及清零区在 CCM。
extern uint8_t _siccmram[];
extern uint8_t _sccmram[];
extern uint8_t _eccmram[];
extern uint8_t _sccmram_bss[];
extern uint8_t _eccmram_bss[];
// 普通 SRAM 中静态区末端到 MSP 栈底之间，是 newlib 可以扩展的 C 堆。
extern uint8_t _end[];
extern uint8_t _sstack[];
static size_t c_heap_peak;

void *__real__sbrk(ptrdiff_t increment);

// 复用原newlib堆边界实现，仅记录实际扩展峰值，不另建分配器。
void *__wrap__sbrk(ptrdiff_t increment)
{
  void *result = __real__sbrk(increment);
  if (result != (void *)-1 && increment > 0)
  {
    // _sbrk 返回扩展前的堆顶；加本次增量得到新堆顶，再换算为占用字节数。
    size_t used = (uintptr_t)result + (size_t)increment - (uintptr_t)_end;
    if (used > c_heap_peak) c_heap_peak = used;
  }
  return result;
}

// 只在阶段边界打印一次；C堆值为分配器地址扩展峰值，不冒充活跃对象字节数。
void AppMemory_Report(const char *phase)
{
  printf("MEM phase=%s c_arena_peak=%lu c_capacity=%lu rtos_min_free=%lu stack_min_free=%lu B\r\n",
         phase, (unsigned long)c_heap_peak,
         (unsigned long)((uintptr_t)_sstack - (uintptr_t)_end),
         (unsigned long)xPortGetMinimumEverFreeHeapSize(),
         (unsigned long)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
}

/*
 * App Reset_Handler 调用 __libc_init_array 时，经下面的 preinit 函数表进入这里。
 * 在构造函数和 main 前补齐 CCM 初始化；没有修改 CCM 容量，也不在这里分配业务资源。
 */
static void AppMemory_InitCcm(void)
{
  // 1. 先打开 CCM 数据 RAM 时钟，后面的复制和清零才能访问其地址。
  __HAL_RCC_CCMDATARAMEN_CLK_ENABLE();
  // 2. 用链接符号计算 CCM 已初始化数据和零初始化区的字节数，末端地址不包含在区间内。
  size_t data_size = (uintptr_t)_eccmram - (uintptr_t)_sccmram;
  size_t bss_size = (uintptr_t)_eccmram_bss - (uintptr_t)_sccmram_bss;
  // 3. CCM 的数据初值从 Flash 复制，CCM 的 bss 写零，补上普通启动汇编没有处理的两段。
  memcpy(_sccmram, _siccmram, data_size);
  memset(_sccmram_bss, 0, bss_size);
}

// 将函数指针放入链接脚本保留的 .preinit_array，由运行库自动调用，无需 main 再手动初始化。
static void (*const ccm_preinit)(void)
  __attribute__((section(".preinit_array"), used)) = AppMemory_InitCcm;

// 串行化任务中的 newlib 分配；调度器启动前无需加锁，ISR 禁止使用 C 堆。
void __malloc_lock(struct _reent *context)
{
  (void)context;
  configASSERT(__get_IPSR() == 0U);
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
    // 暂停任务切换以保护分配器；中断仍可运行，但 ISR 不得调用 malloc/free。
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
