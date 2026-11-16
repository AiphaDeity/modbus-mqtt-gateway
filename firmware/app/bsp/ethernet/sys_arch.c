/*
 * sys_arch.c —— lwIP FreeRTOS 操作系统抽象层移植
 *
 * lwIP 是一个独立的 TCP/IP 协议栈，不依赖特定 OS。
 * 本文件实现 lwIP 要求的 OS 接口（sys_arch.h 定义），把 lwIP 的
 * 互斥锁/信号量/消息队列/线程/时间 等 OS 原语映射到 FreeRTOS 的对应设施。
 *
 * 这是 M2 网络栈的底层依赖——lwIP 协议栈运行在 FreeRTOS 之上，需要本文件桥接。
 *
 * 映射关系：
 *   lwIP 互斥锁 (sys_mutex_t)  → FreeRTOS 递归互斥锁 (xSemaphoreCreateRecursiveMutex)
 *   lwIP 信号量 (sys_sem_t)    → FreeRTOS 二值信号量 (xSemaphoreCreateBinary)
 *   lwIP 消息队列 (sys_mbox_t) → FreeRTOS 队列 (xQueueCreate)
 *   lwIP 线程 (sys_thread_t)   → FreeRTOS 任务 (xTaskCreate)
 *   lwIP 时间 (sys_now)        → FreeRTOS tick 计数 (xTaskGetTickCount)
 *   lwIP 临界区 (sys_arch_protect) → FreeRTOS taskENTER_CRITICAL
 *
 * 本文件基于 lwIP 官方 FreeRTOS 移植模板，适配 STM32F407 + FreeRTOS。
 */
#include "lwip/debug.h"                  // LwIP调试宏，打印调试信息
#include "lwip/def.h"                    // LwIP基础定义，字节序、通用类型
#include "lwip/sys.h"                    // LwIP系统抽象层接口声明，本文件要实现的函数都在这
#include "lwip/mem.h"                    // LwIP内存管理
#include "lwip/stats.h"                  // LwIP统计功能，统计收发、错误数量


// 裸机移植的计时代码，现在用FreeRTOS了，注释掉不用
//extern uint32_t lwip_localtime;
//u32_t sys_now(void){
//	return lwip_localtime;
//}

#include "FreeRTOS.h"                    // FreeRTOS内核主头文件
#include "semphr.h"                      // FreeRTOS信号量、互斥锁API
#include "task.h"                        // FreeRTOS任务、tick相关API

int errno;                                // 全局错误号变量，标准C库用

// 栈大小单位开关：1=按FreeRTOS的"字"算（32位一个字=4字节），0=按LwIP的字节算
// 设成1，因为FreeRTOS创建任务的栈参数本来就是按字数的
#ifndef LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS   // 没定义过才定义
#define LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS  1   // 栈单位=FreeRTOS字
#endif                                            // 条件编译结束

// 临界区实现方式：1=用互斥锁，0=关中断/关调度器，默认0，用关中断更简单
#ifndef LWIP_FREERTOS_SYS_ARCH_PROTECT_USES_MUTEX   // 没定义过才定义
#define LWIP_FREERTOS_SYS_ARCH_PROTECT_USES_MUTEX     0   // 0=关中断实现临界区
#endif                                            // 条件编译结束

// 临界区配对检查：1=检查保护和解锁是不是成对调用，调试用，默认关
#ifndef LWIP_FREERTOS_SYS_ARCH_PROTECT_SANITY_CHECK   // 没定义过才定义
#define LWIP_FREERTOS_SYS_ARCH_PROTECT_SANITY_CHECK   0   // 0=关配对检查
#endif                                            // 条件编译结束

// 释放消息队列时检查队列是不是空的，1=开检查，默认关
#ifndef LWIP_FREERTOS_CHECK_QUEUE_EMPTY_ON_FREE       // 没定义过才定义
#define LWIP_FREERTOS_CHECK_QUEUE_EMPTY_ON_FREE       0   // 0=关队列空检查
#endif                                            // 条件编译结束

// 内核锁检查，1=开核心锁断言，需要在lwipopts里配相关宏，默认关
#ifndef LWIP_FREERTOS_CHECK_CORE_LOCKING              // 没定义过才定义
#define LWIP_FREERTOS_CHECK_CORE_LOCKING              0   // 0=关核心锁检查
#endif                                            // 条件编译结束

// 时间来源：1=用FreeRTOS的tick算毫秒，0=自己用硬件定时器实现，默认1
#ifndef LWIP_FREERTOS_SYS_NOW_FROM_FREERTOS           // 没定义过才定义
#define LWIP_FREERTOS_SYS_NOW_FROM_FREERTOS           1   // 1=时间取自FreeRTOS
#endif                                            // 条件编译结束

// 编译前检查：FreeRTOS必须开启动态内存分配，不然编译直接报错
#if !configSUPPORT_DYNAMIC_ALLOCATION              // 没开动态分配
# error "lwIP FreeRTOS port requires configSUPPORT_DYNAMIC_ALLOCATION"   // 编译报错
#endif                                            // 条件编译结束
// 必须开启vTaskDelay延时函数
#if !INCLUDE_vTaskDelay                           // 没开延时函数
# error "lwIP FreeRTOS port requires INCLUDE_vTaskDelay"   // 编译报错
#endif                                            // 条件编译结束
// 必须开启任务挂起功能
#if !INCLUDE_vTaskSuspend                        // 没开任务挂起
# error "lwIP FreeRTOS port requires INCLUDE_vTaskSuspend"   // 编译报错
#endif                                            // 条件编译结束
// 用互斥锁或者不用兼容模式的话，必须开FreeRTOS的互斥锁
#if LWIP_FREERTOS_SYS_ARCH_PROTECT_USES_MUTEX || !LWIP_COMPAT_MUTEX   // 需要互斥锁
#if !configUSE_MUTEXES                            // 没开互斥锁
# error "lwIP FreeRTOS port requires configUSE_MUTEXES"   // 编译报错
#endif                                            // 内层条件编译结束
#endif                                            // 外层条件编译结束

// 开轻量保护且用互斥锁的话，定义一个临界区专用的互斥锁句柄
#if SYS_LIGHTWEIGHT_PROT && LWIP_FREERTOS_SYS_ARCH_PROTECT_USES_MUTEX   // 开了轻量保护+互斥锁
static SemaphoreHandle_t sys_arch_protect_mutex;   // 临界区专用互斥锁
#endif                                            // 条件编译结束
// 开配对检查的话，定义嵌套层数变量，检查进出平衡
#if SYS_LIGHTWEIGHT_PROT && LWIP_FREERTOS_SYS_ARCH_PROTECT_SANITY_CHECK   // 开了配对检查
static sys_prot_t sys_arch_protect_nesting;       // 嵌套计数
#endif                                            // 条件编译结束

// 函数：sys_init —— OS抽象层初始化
// 功能：LwIP移植层初始化入口，tcpip_init时LwIP自动调：本平台没有需要初始化的东西（FreeRTOS和ETH驱动已就绪），留空实现保证移植接口完整
// 参数：无
// 返回：void，无返回值
void // 返回类型void，无返回值
sys_init(void)                                    // OS抽象层初始化
{
#if SYS_LIGHTWEIGHT_PROT && LWIP_FREERTOS_SYS_ARCH_PROTECT_USES_MUTEX   // 用互斥锁才有内容
  // 用互斥锁的话，这里创建递归互斥锁，断言创建成功
  sys_arch_protect_mutex = xSemaphoreCreateRecursiveMutex();   // 建递归互斥锁
  LWIP_ASSERT("failed to create sys_arch_protect mutex",   // 断言文本
    sys_arch_protect_mutex != NULL);              // 句柄不能空
#endif                                            // 条件编译结束
}                                                 // 初始化结束

// 必须用32位tick，16位的话溢出太快，时间管理会出问题
#if configUSE_16_BIT_TICKS == 1                   // 配成16位tick
#error This port requires 32 bit ticks or timer overflow will fail   // 编译报错
#endif                                            // 条件编译结束

// 返回当前毫秒时间戳，LwIP算超时用，tick数乘以每个tick的毫秒数
// 函数：sys_now —— 当前毫秒时间戳
// 功能：给LwIP内核提供“现在过了多少毫秒”：直接读FreeRTOS的tick计数乘以每tick毫秒数换算；LwIP的定时器（TCP重传、ARP老化）全靠这个时间基准
// 参数：无
// 返回：u32_t，开机至今的毫秒数（32位约49天回绕一次，LwIP内部做了差值比较不怕回绕）
#if LWIP_FREERTOS_SYS_NOW_FROM_FREERTOS // 时间取自FreeRTOS才编译sys_now
u32_t // 返回32位无符号毫秒数
sys_now(void)                                     // 当前毫秒时间戳
{
  return xTaskGetTickCount() * portTICK_PERIOD_MS;   // tick数×每tick毫秒数
}
#endif                                            // 时间来源条件编译结束

// 函数：sys_jiffies —— 当前tick计数
// 功能：返回原始tick计数（不换算毫秒），给LwIP做粗粒度时间戳用，开销比sys_now更小；本移植和sys_now同源
// 参数：无
// 返回：u32_t，FreeRTOS的原始tick计数值
// 返回当前tick计数，LwIP内部时间统计用
u32_t // 返回32位无符号tick数
sys_jiffies(void)                                 // 当前tick计数
{
  return xTaskGetTickCount();                     // 直接返回tick值
}

#if SYS_LIGHTWEIGHT_PROT                          // 开轻量保护才编译临界区

// 函数：sys_arch_protect —— 进入临界区
// 功能：LwIP保护共享数据时调：进FreeRTOS临界区（关可屏蔽中断+锁定调度），多任务和中断都不能再打断，防LwIP内核数据被踩；注意临界区必须极短
// 参数：无
// 返回：sys_prot_t，临界区前的状态标记，退出时必须原样传给sys_arch_unprotect
sys_prot_t // 返回保护标记类型
sys_arch_protect(void)                            // 进入临界区
{
#if LWIP_FREERTOS_SYS_ARCH_PROTECT_USES_MUTEX     // 用互斥锁实现
  BaseType_t ret;                                 // 拿锁结果
  LWIP_ASSERT("sys_arch_protect_mutex != NULL", sys_arch_protect_mutex != NULL);   // 锁必须已建

  // 用互斥锁的话，递归拿锁，一直等
  ret = xSemaphoreTakeRecursive(sys_arch_protect_mutex, portMAX_DELAY);   // 无限等拿锁
  LWIP_ASSERT("sys_arch_protect failed to take the mutex", ret == pdTRUE);   // 必须成功
#else                                             // 不用锁走关中断
  taskENTER_CRITICAL();                    // 不用互斥锁，直接进临界区关中断
#endif                                            // 条件编译结束
#if LWIP_FREERTOS_SYS_ARCH_PROTECT_SANITY_CHECK   // 开配对检查
  {
    // 开配对检查的话，嵌套层数加1，返回之前的层数，检查溢出
    sys_prot_t ret = sys_arch_protect_nesting;    // 记录旧层数
    sys_arch_protect_nesting++;                  // 层数加1
    LWIP_ASSERT("sys_arch_protect overflow", sys_arch_protect_nesting > ret);   // 溢出检查
    return ret;                                  // 返回旧层数
  }
#else                                             // 没开检查
  return 1;                                 // 没开检查，随便返回1就行
#endif                                            // 条件编译结束
}

// 函数：sys_arch_unprotect —— 退出临界区
// 功能：和sys_arch_protect配对：把protect时保存的状态标记传回，恢复中断和调度，临界区结束；LwIP内核锁机制在无专用内核锁时靠这对函数兜底
// 参数：pval —— 进入临界区时sys_arch_protect返回的状态标记，原样传回
// 返回：void，无返回值
void // 返回类型void
sys_arch_unprotect(sys_prot_t pval)               // 退出临界区
{
#if LWIP_FREERTOS_SYS_ARCH_PROTECT_USES_MUTEX     // 用锁才有局部变量
  BaseType_t ret;                                 // 放锁结果
#endif                                            // 条件编译结束
#if LWIP_FREERTOS_SYS_ARCH_PROTECT_SANITY_CHECK   // 开配对检查
  // 配对检查：层数减1，确认和传入的标记一致
  LWIP_ASSERT("unexpected sys_arch_protect_nesting", sys_arch_protect_nesting > 0);   // 层数不能0
  sys_arch_protect_nesting--;                    // 层数减1
  LWIP_ASSERT("unexpected sys_arch_protect_nesting", sys_arch_protect_nesting == pval);   // 必须配对
#endif                                            // 条件编译结束

#if LWIP_FREERTOS_SYS_ARCH_PROTECT_USES_MUTEX     // 用锁走放锁
  LWIP_ASSERT("sys_arch_protect_mutex != NULL", sys_arch_protect_mutex != NULL);   // 锁必须存在

  // 用互斥锁的话，递归释放锁
  ret = xSemaphoreGiveRecursive(sys_arch_protect_mutex);   // 递归放锁
  LWIP_ASSERT("sys_arch_unprotect failed to give the mutex", ret == pdTRUE);   // 必须成功
#else                                             // 不用锁走开中断
  taskEXIT_CRITICAL();                     // 不用锁，直接退出临界区开中断
#endif                                            // 条件编译结束
  LWIP_UNUSED_ARG(pval);                   // 标记参数没用到，避免警告
}

#endif // SYS_LIGHTWEIGHT_PROT 轻量保护条件编译结束

// 函数：sys_arch_msleep —— 毫秒睡眠
// 功能：LwIP需要毫秒级睡眠时调：把毫秒换算成tick后调FreeRTOS的osDelay交出CPU，让别的任务跑；睡眠期间不占CPU不忙等
// 参数：delay_ms —— 要睡眠的毫秒数
// 返回：void，无返回值，睡够指定时长后返回
void // 返回类型void
sys_arch_msleep(u32_t delay_ms)                   // 毫秒睡眠
{
  TickType_t delay_ticks = delay_ms / portTICK_RATE_MS;   // 毫秒转tick数
  vTaskDelay(delay_ticks);                        // FreeRTOS延时
}

#if !LWIP_COMPAT_MUTEX                            // 不用兼容锁才实现这些

// 函数：sys_mutex_new —— 创建互斥锁
// 功能：把LwIP的互斥锁创建请求转成FreeRTOS互斥锁：用osMutexNew建优先级继承互斥锁（防优先级翻转），创建结果写进LwIP的锁结构；失败时给空值让LwIP能识别
// 参数：mutex —— LwIP互斥锁结构指针，FreeRTOS句柄存在它里面
// 返回：err_t，ERR_OK代表创建成功；ERR_MEM代表FreeRTOS锁资源不够
err_t // 返回err_t错误码
sys_mutex_new(sys_mutex_t *mutex)                 // 创建互斥锁
{
  LWIP_ASSERT("mutex != NULL", mutex != NULL);   // 断言指针非空

  mutex->mut = xSemaphoreCreateRecursiveMutex();  // 创建递归互斥锁
  if(mutex->mut == NULL) {                        // 创建失败
    SYS_STATS_INC(mutex.err);                    // 统计错误数
    return ERR_MEM;                              // 内存不够，创建失败
  }
  SYS_STATS_INC_USED(mutex);                     // 统计在用数量
  return ERR_OK;                                 // 创建成功
}

// 函数：sys_mutex_lock —— 加锁
// 功能：给临界段加锁：调osMutexAcquire死等拿到FreeRTOS互斥锁，别的任务想拿同一把锁就得睡眠排队；LwIP内核锁和MEM锁都走它
// 参数：mutex —— 要加的互斥锁结构指针（内含FreeRTOS锁句柄）
// 返回：void，无返回值，拿到锁才返回
void // 返回类型void
sys_mutex_lock(sys_mutex_t *mutex)                // 加锁
{
  BaseType_t ret;                                 // 拿锁结果
  LWIP_ASSERT("mutex != NULL", mutex != NULL);   // 指针非空
  LWIP_ASSERT("mutex->mut != NULL", mutex->mut != NULL);   // 句柄非空

  ret = xSemaphoreTakeRecursive(mutex->mut, portMAX_DELAY);  // 无限等待拿锁
  LWIP_ASSERT("failed to take the mutex", ret == pdTRUE);   // 必须成功
}

// 函数：sys_mutex_unlock —— 解锁
// 功能：临界段结束释放锁：调osMutexRelease让FreeRTOS唤醒排在后面的任务；必须和sys_mutex_lock严格配对，多解少解都会把系统搞挂
// 参数：mutex —— 要释放的互斥锁结构指针
// 返回：void，无返回值
void // 返回类型void
sys_mutex_unlock(sys_mutex_t *mutex)              // 解锁
{
  BaseType_t ret;                                 // 放锁结果
  LWIP_ASSERT("mutex != NULL", mutex != NULL);   // 指针非空
  LWIP_ASSERT("mutex->mut != NULL", mutex->mut != NULL);   // 句柄非空

  ret = xSemaphoreGiveRecursive(mutex->mut);     // 释放递归锁
  LWIP_ASSERT("failed to give the mutex", ret == pdTRUE); // 递归放锁必须成功
}

// 函数：sys_mutex_free —— 删除互斥锁
// 功能：锁彻底不再用时删掉：调osMutexDelete释放FreeRTOS互斥锁资源并把句柄置空；LwIP关连接池等清理路径会调
// 参数：mutex —— 要删除的互斥锁结构指针，删完句柄清空
// 返回：void，无返回值
void // 返回类型void
sys_mutex_free(sys_mutex_t *mutex)                 // 删除互斥锁
{
  LWIP_ASSERT("mutex != NULL", mutex != NULL);     // 指针非空
  LWIP_ASSERT("mutex->mut != NULL", mutex->mut != NULL);   // 句柄非空

  SYS_STATS_DEC(mutex.used);                     // 在用数量减1
  vSemaphoreDelete(mutex->mut);                  // 删除信号量
  mutex->mut = NULL;                             // 句柄置空
}

#endif // !LWIP_COMPAT_MUTEX 互斥锁条件编译结束

// ===== LwIP信号量，映射到FreeRTOS二值信号量 =====

// 函数：sys_sem_new —— 创建信号量
// 功能：把LwIP信号量转成FreeRTOS计数信号量：按初始计数调osSemaphoreNew创建；LwIP内核用它同步（tcpip消息、netconn收发事件）；创建失败置空并断言拦截
// 参数：sem —— LwIP信号量结构指针，句柄存这里；initial_count —— 初始计数值（一般1表示初始可用，0表示要先等）
// 返回：err_t，ERR_OK代表创建成功；ERR_MEM代表FreeRTOS信号量资源不够
err_t // 返回err_t错误码
sys_sem_new(sys_sem_t *sem, u8_t initial_count)   // 创建信号量
{
  LWIP_ASSERT("sem != NULL", sem != NULL);        // 指针非空
  LWIP_ASSERT("initial_count invalid (not 0 or 1)",   // 断言文本
    (initial_count == 0) || (initial_count == 1));  // 只能是0或1

  sem->sem = xSemaphoreCreateBinary();             // 创建二值信号量
  if(sem->sem == NULL) {                           // 创建失败
    SYS_STATS_INC(sem.err);                        // 统计错误
    return ERR_MEM;                                // 内存不足
  }
  SYS_STATS_INC_USED(sem);                         // 统计在用

  if(initial_count == 1) {                         // 初始就要可用
    BaseType_t ret = xSemaphoreGive(sem->sem);     // 初始可用，先释放一次
    LWIP_ASSERT("sys_sem_new: initial give failed", ret == pdTRUE);   // 必须成功
  }
  return ERR_OK;                                   // 创建成功
}

// 函数：sys_sem_signal —— 发信号量
// 功能：释放信号量让等待方醒来：调osSemaphoreRelease把计数加一；LwIP内核处理完消息、netconn收到数据都会调它唤醒等结果的线程，也常在ISR外的事件通知里用
// 参数：sem —— 要发的信号量结构指针
// 返回：void，无返回值
void // 返回类型void
sys_sem_signal(sys_sem_t *sem)                     // 发信号量
{
  BaseType_t ret;                                 // 释放结果
  LWIP_ASSERT("sem != NULL", sem != NULL);        // 指针非空
  LWIP_ASSERT("sem->sem != NULL", sem->sem != NULL);   // 句柄非空

  ret = xSemaphoreGive(sem->sem);                 // 释放信号量
  LWIP_ASSERT("sys_sem_signal: sane return value",   // 断言文本
    (ret == pdTRUE) || (ret == errQUEUE_FULL));   // 成功或已满都算正常
}

// 函数：sys_arch_sem_wait —— 等信号量
// 功能：限时等信号量：调osSemaphoreAcquire带超时等；拿到返回0，超时返回SYS_ARCH_TIMEOUT，LwIP的netconn收发、tcpip同步全靠它；timeout传0表示非阻塞试一次
// 参数：sem —— 要等的信号量结构指针；timeout_ms —— 最长等待毫秒数，0表示不等待只试一次，0xFFFFFFFF表示死等
// 返回：u32_t，0代表拿到信号量；SYS_ARCH_TIMEOUT代表超时没拿到
u32_t // 返回u32_t等待结果
sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout_ms)   // 等信号量
{
  BaseType_t ret;                                 // 拿锁结果
  LWIP_ASSERT("sem != NULL", sem != NULL);        // 指针非空
  LWIP_ASSERT("sem->sem != NULL", sem->sem != NULL);   // 句柄非空

  if(!timeout_ms) {                                // 超时为0
    // 超时0，无限等待
    ret = xSemaphoreTake(sem->sem, portMAX_DELAY);   // 无限等拿信号量
    LWIP_ASSERT("taking semaphore failed", ret == pdTRUE);   // 必须成功
  } else {                                        // 有超时
    TickType_t timeout_ticks = timeout_ms / portTICK_RATE_MS;  // 毫秒转tick
    ret = xSemaphoreTake(sem->sem, timeout_ticks);  // 限时拿信号量
    if (ret == errQUEUE_EMPTY) {                  // 没拿到超时了
      return SYS_ARCH_TIMEOUT;                  // 超时了，返回超时标记
    }
    LWIP_ASSERT("taking semaphore failed", ret == pdTRUE);   // 其他失败不允许
  }

  // 现在不需要返回具体等待时间，只要不是超时就行，返回1
  return 1;                                       // 非超时返回1
}

// 函数：sys_sem_free —— 删除信号量
// 功能：信号量不再用时删掉：调osSemaphoreDelete释放FreeRTOS资源并置空句柄，LwIP删除netconn等对象时清理配套信号量
// 参数：sem —— 要删除的信号量结构指针
// 返回：void，无返回值
void // 返回类型void
sys_sem_free(sys_sem_t *sem)                      // 删除信号量
{
  LWIP_ASSERT("sem != NULL", sem != NULL);        // 指针非空
  LWIP_ASSERT("sem->sem != NULL", sem->sem != NULL);   // 句柄非空

  SYS_STATS_DEC(sem.used);                      // 在用数量减1
  vSemaphoreDelete(sem->sem);                   // 删除信号量
  sem->sem = NULL;                              // 句柄置空
}

// ===== LwIP消息队列，映射到FreeRTOS队列，线程间传递数据包和回调 =====

// 函数：sys_mbox_new —— 创建消息队列
// 功能：把LwIP邮箱转成FreeRTOS消息队列：调osMessageQueueNew按size建队列（每格存一个指针）；LwIP内核线程和tcpip_init邮箱、netconn收包队列全走它；失败返回错误码让LwIP知道
// 参数：mbox —— LwIP邮箱结构指针，队列句柄存这里；size —— 队列深度（能存几条消息指针）
// 返回：err_t，ERR_OK代表创建成功；ERR_MEM代表FreeRTOS队列资源不够
err_t // 返回err_t错误码
sys_mbox_new(sys_mbox_t *mbox, int size)          // 创建消息队列
{
  LWIP_ASSERT("mbox != NULL", mbox != NULL);      // 指针非空
  LWIP_ASSERT("size > 0", size > 0);              // 长度必须正

  mbox->mbx = xQueueCreate((UBaseType_t)size, sizeof(void *));  // 创建队列，元素大小是指针大小
  if(mbox->mbx == NULL) {                         // 创建失败
    SYS_STATS_INC(mbox.err);                    // 统计错误
    return ERR_MEM;                             // 内存不足
  }
  SYS_STATS_INC_USED(mbox);                     // 统计在用
  return ERR_OK;                                // 创建成功
}

// 函数：sys_mbox_post —— 阻塞式发消息
// 功能：往邮箱发消息，满了就死等：调osMessageQueuePut超时无限大，队列满时调用线程睡眠直到有空位；LwIP用它保证消息必达（如tcpip消息投递）
// 参数：mbox —— 目标邮箱结构指针；msg —— 消息指针（一般是pbuf或事件指针，LwIP靠指针传数据）
// 返回：void，无返回值，消息必进队列才返回
void // 返回类型void
sys_mbox_post(sys_mbox_t *mbox, void *msg)        // 阻塞式发消息
{
  BaseType_t ret;                                 // 发送结果
  LWIP_ASSERT("mbox != NULL", mbox != NULL);      // 指针非空
  LWIP_ASSERT("mbox->mbx != NULL", mbox->mbx != NULL);   // 句柄非空

  ret = xQueueSendToBack(mbox->mbx, &msg, portMAX_DELAY);  // 无限等待发送
  LWIP_ASSERT("mbox post failed", ret == pdTRUE);   // 必须成功
}

// 函数：sys_mbox_trypost —— 非阻塞发消息
// 功能：往邮箱发消息但队列满了不等待：调osMessageQueuePut超时0立即返回；满则返回ERR_MEM让LwIP知道消息没进来（LwIP会丢包或报错），API层超时控制用它
// 参数：mbox —— 目标邮箱结构指针；msg —— 消息指针
// 返回：err_t，ERR_OK代表已入队；ERR_MEM代表队列满没发进去
err_t // 返回err_t错误码
sys_mbox_trypost(sys_mbox_t *mbox, void *msg)     // 非阻塞发消息
{
  BaseType_t ret;                                 // 发送结果
  LWIP_ASSERT("mbox != NULL", mbox != NULL);      // 指针非空
  LWIP_ASSERT("mbox->mbx != NULL", mbox->mbx != NULL);   // 句柄非空

  ret = xQueueSendToBack(mbox->mbx, &msg, 0);   // 超时0，立刻返回
  if (ret == pdTRUE) {                            // 发送成功
    return ERR_OK;                              // 成功
  } else {                                        // 发送失败
    LWIP_ASSERT("mbox trypost failed", ret == errQUEUE_FULL);   // 只能是满
    SYS_STATS_INC(mbox.err);                    // 统计错误
    return ERR_MEM;                             // 队列满了
  }
}

// 函数：sys_mbox_trypost_fromisr —— 中断版发消息
// 功能：中断服务程序里往邮箱发消息的专用版：LwIP开了SYS_LIGHT_WEIGHT_PROT时中断里也能投递（如ETH中断唤醒）；本FreeRTOS移植里消息队列本身中断安全，实现转调trypost风格逻辑
// 参数：mbox —— 目标邮箱结构指针；msg —— 消息指针
// 返回：err_t，ERR_OK代表已入队；ERR_MEM代表队列满没发进去
err_t // 返回err_t错误码
sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)   // 中断版发消息
{
  BaseType_t ret;                                 // 发送结果
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;  // 标记退出中断后要不要切换任务
  LWIP_ASSERT("mbox != NULL", mbox != NULL);      // 指针非空
  LWIP_ASSERT("mbox->mbx != NULL", mbox->mbx != NULL);   // 句柄非空

  ret = xQueueSendToBackFromISR(mbox->mbx, &msg, &xHigherPriorityTaskWoken);   // ISR版发送
  if (ret == pdTRUE) {                            // 发送成功
    if (xHigherPriorityTaskWoken == pdTRUE) {     // 唤醒了更高优先级任务
      return ERR_NEED_SCHED;               // 有更高优先级任务唤醒了，需要调度
    }
    return ERR_OK;                                // 正常成功
  } else {                                        // 发送失败
    LWIP_ASSERT("mbox trypost failed", ret == errQUEUE_FULL);   // 只能是满
    SYS_STATS_INC(mbox.err);                      // 统计错误
    return ERR_MEM;                        // 队列满了
  }
}

// 函数：sys_arch_mbox_fetch —— 阻塞取消息
// 功能：从邮箱限时取一条消息：调osMessageQueueGet带超时；取到返回0并把消息指针写给出参，超时返回SYS_ARCH_TIMEOUT；netconn收包、tcpip线程收指令的主等待全靠它，是LwIP最热的等待点
// 参数：mbox —— 源邮箱结构指针；msg —— 出参，取到的消息指针写到这里；timeout_ms —— 最长等待毫秒数，0表示非阻塞试一次，0xFFFFFFFF死等
// 返回：u32_t，0代表取到消息；SYS_ARCH_TIMEOUT代表超时没消息
u32_t // 返回u32_t等待结果
sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout_ms)   // 阻塞取消息
{
  BaseType_t ret;                                 // 接收结果
  void *msg_dummy;                                // 临时消息指针
  LWIP_ASSERT("mbox != NULL", mbox != NULL);      // 指针非空
  LWIP_ASSERT("mbox->mbx != NULL", mbox->mbx != NULL);   // 句柄非空

  if (!msg) {                                      // 调用方不要消息
    msg = &msg_dummy;                         // 没传接收指针，用临时变量
  }

  if (!timeout_ms) {                               // 超时0无限等
    // 无限等待
    ret = xQueueReceive(mbox->mbx, &(*msg), portMAX_DELAY);   // 无限等收
    LWIP_ASSERT("mbox fetch failed", ret == pdTRUE);   // 必须成功
  } else {                                         // 有超时
    TickType_t timeout_ticks = timeout_ms / portTICK_RATE_MS;  // 毫秒转tick
    ret = xQueueReceive(mbox->mbx, &(*msg), timeout_ticks);   // 限时收
    if (ret == errQUEUE_EMPTY) {                   // 超时没收到
      *msg = NULL;                                // 消息置空
      return SYS_ARCH_TIMEOUT;               // 超时，消息置空，返回超时
    }
    LWIP_ASSERT("mbox fetch failed", ret == pdTRUE);   // 其他失败不允许
  }

  // 不需要返回等待时间，非超时就行
  return 1;                                        // 非超时返回1
}

// 函数：sys_arch_mbox_tryfetch —— 非阻塞取消息
// 功能：从邮箱取一条消息但不等待：调osMessageQueueGet超时0；有消息返回0并写指针，空返回SYS_ARCH_TIMEOUT；netconn轮询模式、清理残留消息时用
// 参数：mbox —— 源邮箱结构指针；msg —— 出参，取到的消息指针写到这里（没取到不动）
// 返回：u32_t，0代表取到消息；SYS_ARCH_TIMEOUT代表队列空没取到
u32_t // 返回u32_t等待结果
sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)   // 非阻塞取消息
{
  BaseType_t ret;                                 // 接收结果
  void *msg_dummy;                                // 临时消息指针
  LWIP_ASSERT("mbox != NULL", mbox != NULL);      // 指针非空
  LWIP_ASSERT("mbox->mbx != NULL", mbox->mbx != NULL);   // 句柄非空

  if (!msg) {                                      // 调用方不要消息
    msg = &msg_dummy;                         // 没传接收指针，用临时变量
  }

  ret = xQueueReceive(mbox->mbx, &(*msg), 0);  // 超时0，立刻返回
  if (ret == errQUEUE_EMPTY) {                    // 队列是空的
    *msg = NULL;                                  // 消息置空
    return SYS_MBOX_EMPTY;                   // 队列空，返回空标记
  }
  LWIP_ASSERT("mbox fetch failed", ret == pdTRUE);   // 其他失败不允许

  return 1;                                       // 非空返回1
}

// 函数：sys_mbox_free —— 删除消息队列
// 功能：邮箱不再用时删掉：先断言队列必须已空（非空删除会漏消息），再调osMessageQueueDelete释放FreeRTOS队列资源并置空句柄；LwIP关netconn、销毁内核时清理
// 参数：mbox —— 要删除的邮箱结构指针
// 返回：void，无返回值
void // 返回类型void
sys_mbox_free(sys_mbox_t *mbox)                   // 删除消息队列
{
  LWIP_ASSERT("mbox != NULL", mbox != NULL);      // 指针非空
  LWIP_ASSERT("mbox->mbx != NULL", mbox->mbx != NULL);   // 句柄非空

#if LWIP_FREERTOS_CHECK_QUEUE_EMPTY_ON_FREE       // 开了队列空检查
  {
    // 开检查的话，先看队列里还有没有消息，断言是空的
    UBaseType_t msgs_waiting = uxQueueMessagesWaiting(mbox->mbx);   // 查积压消息数
    LWIP_ASSERT("mbox quence not empty", msgs_waiting == 0);   // 必须是空

    if (msgs_waiting != 0) {                      // 还有积压
      SYS_STATS_INC(mbox.err);               // 非空统计错误
    }
  }
#endif                                            // 条件编译结束

  vQueueDelete(mbox->mbx);                    // 删除队列

  SYS_STATS_DEC(mbox.used);                   // 在用数量减1
}

// 函数：sys_thread_new —— 创建线程
// 功能：把LwIP建线程请求转成FreeRTOS任务：给线程起名字、分配栈、按LwIP优先级映射到FreeRTOS优先级后osThreadNew创建；tcpip内核线程、netconn相关线程都从这建，栈大小用LwIP推荐值
// 参数：name —— 线程名字符串（调试用）；thread —— 线程入口函数指针；arg —— 传给入口函数的参数；stacksize —— 栈大小字节数；prio —— LwIP线程优先级（会映射成CMSIS优先级）
// 返回：sys_thread_t，创建成功的任务句柄；失败返回NULL，LwIP会中止启动
// ===== LwIP线程创建，映射到FreeRTOS任务，tcpip和netconn线程都用这个创建
sys_thread_t // 返回LwIP线程句柄
sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio)   // 创建线程
{
  TaskHandle_t rtos_task = NULL;                  // FreeRTOS任务句柄
  BaseType_t ret;                                 // 创建结果
  sys_thread_t lwip_thread;                       // LwIP线程结构
  size_t rtos_stacksize;                          // 换算后的栈大小

  LWIP_ASSERT("invalid stacksize", stacksize > 0);   // 栈必须正数
#if LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS   // 栈单位是字
  rtos_stacksize = (size_t)stacksize;        // 栈单位是字，直接用
#else                                             // 栈单位是字节
  rtos_stacksize = (size_t)stacksize / sizeof(StackType_t);  // 字节转字数
#endif                                            // 条件编译结束

  // 线程函数类型和FreeRTOS的任务函数类型一致，直接传
  ret = xTaskCreate(thread, name, (configSTACK_DEPTH_TYPE)rtos_stacksize, arg, prio, &rtos_task);   // 创建任务
  LWIP_ASSERT("task creation failed", ret == pdTRUE);   // 必须成功

  lwip_thread.thread_handle = rtos_task;      // 把任务句柄包进LwIP的线程结构
  return lwip_thread;                             // 返回LwIP线程结构
}

#if LWIP_NETCONN_SEM_PER_THREAD                   // 开每线程netconn信号量
#if configNUM_THREAD_LOCAL_STORAGE_POINTERS > 0    // 本地存储指针数够

// 函数：sys_arch_netconn_sem_get —— 取当前任务的netconn信号量
// 功能：netconn API的多任务支持：每个任务拥有自己的收包信号量，LwIP内核收到属于该任务的数据时调它拿信号量去释放；通过任务本地存储（TLS）按当前线程找到对应信号量指针
// 参数：无（按当前运行的任务查）
// 返回：sys_sem_t*，当前任务的netconn信号量指针；找不到返回NULL
sys_sem_t * // 返回信号量指针
sys_arch_netconn_sem_get(void)                    // 取当前任务的信号量
{
  void* ret;                                     // 取出的指针
  TaskHandle_t task = xTaskGetCurrentTaskHandle();   // 当前任务句柄
  LWIP_ASSERT("task != NULL", task != NULL);      // 句柄非空

  ret = pvTaskGetThreadLocalStoragePointer(task, 0);  // 读本地存储0号槽
  return ret;                                     // 返回信号量指针
}

// 函数：sys_arch_netconn_sem_alloc —— 分配netconn信号量
// 功能：任务第一次调netconn API时LwIP自动调：给当前任务创建一个专属信号量（osSemaphoreNew初值0）并挂到任务本地存储里，之后收包等待都等它；一个任务只建一次
// 参数：无（创建结果挂到当前任务的TLS里）
// 返回：void，无返回值，失败会断言拦截
void // 返回类型void
sys_arch_netconn_sem_alloc(void)                  // 分配netconn信号量
{
  void *ret;                                     // 查询结果
  TaskHandle_t task = xTaskGetCurrentTaskHandle();   // 当前任务句柄
  LWIP_ASSERT("task != NULL", task != NULL);      // 句柄非空

  ret = pvTaskGetThreadLocalStoragePointer(task, 0);   // 查本地存储0号槽
  if(ret == NULL) {                               // 还没分配过
    sys_sem_t *sem;                               // 信号量指针
    err_t err;                                    // 创建结果
    // 本地存储是空的，先申请内存
    sem = mem_malloc(sizeof(sys_sem_t));          // 申请信号量内存
    LWIP_ASSERT("sem != NULL", sem != NULL);      // 必须成功
    err = sys_sem_new(sem, 0);                 // 创建信号量，初始不可用
    LWIP_ASSERT("err == ERR_OK", err == ERR_OK);   // 必须成功
    LWIP_ASSERT("sem invalid", sys_sem_valid(sem));   // 必须有效
    vTaskSetThreadLocalStoragePointer(task, 0, sem);  // 存进本地存储
  }
}

// 函数：sys_arch_netconn_sem_free —— 释放netconn信号量
// 功能：任务退出或netconn资源清理时LwIP调：把当前任务的netconn信号量删除（osSemaphoreDelete）、释放TLS槽位、清空本地存储，防止任务句柄泄漏；netconn API用完必须配对调用
// 参数：无（操作当前任务的TLS槽位）
// 返回：void，无返回值
void sys_arch_netconn_sem_free(void)              // 释放netconn信号量
{
  void* ret;                                     // 查询结果
  TaskHandle_t task = xTaskGetCurrentTaskHandle();   // 当前任务句柄
  LWIP_ASSERT("task != NULL", task != NULL);      // 句柄非空

  ret = pvTaskGetThreadLocalStoragePointer(task, 0);   // 查本地存储0号槽
  if(ret != NULL) {                               // 有分配过
    sys_sem_t *sem = ret;                         // 转成信号量指针
    sys_sem_free(sem);                            // 删信号量
    mem_free(sem);                                // 释放内存
    vTaskSetThreadLocalStoragePointer(task, 0, NULL);   // 清本地存储
  }
}

#else                                             // 本地存储没配
#error LWIP_NETCONN_SEM_PER_THREAD needs configNUM_THREAD_LOCAL_STORAGE_POINTERS   // 编译报错
#endif                                            // 内层条件编译结束

#endif // LWIP_NETCONN_SEM_PER_THREAD 每线程信号量条件编译结束

#if LWIP_FREERTOS_CHECK_CORE_LOCKING              // 开核心锁检查
#if LWIP_TCPIP_CORE_LOCKING                       // 开核心锁模式

// 核心锁嵌套计数，记录拿了几次
static u8_t lwip_core_lock_count;                 // 嵌套计数
// 持有核心锁的任务句柄
static TaskHandle_t lwip_core_lock_holder_thread;   // 持有锁的任务句柄

// 函数：sys_lock_tcpip_core —— 加LwIP内核大锁
// 功能：LwIP NO_SYS=0+单内核锁模式下，进内核数据区前抓全局核心锁：转成拿FreeRTOS互斥锁；拿到锁的任务独占协议栈内核，别的任务想进就得排队，简单粗暴地防并发踩内核
// 参数：无（锁在全局）
// 返回：void，无返回值，拿到锁才返回
void // 返回类型void
sys_lock_tcpip_core(void)                          // 加核心锁
{
   sys_mutex_lock(&lock_tcpip_core);               // 拿tcpip核心互斥锁
   if (lwip_core_lock_count == 0) {                // 第一次拿
     lwip_core_lock_holder_thread = xTaskGetCurrentTaskHandle();  // 第一次拿，记录持有者
   }
   lwip_core_lock_count++;                         // 嵌套计数加1
}

// 函数：sys_unlock_tcpip_core —— 解LwIP内核大锁
// 功能：内核操作完释放全局核心锁：转成放FreeRTOS互斥锁，让排队等待的任务进来；必须和sys_lock_tcpip_core严格配对使用
// 参数：无
// 返回：void，无返回值
void // 返回类型void
sys_unlock_tcpip_core(void)                        // 解核心锁
{
   lwip_core_lock_count--;                         // 嵌套计数减1
   if (lwip_core_lock_count == 0) {                // 减到0了
       lwip_core_lock_holder_thread = 0;           // 清持有者句柄
   }
   sys_mutex_unlock(&lock_tcpip_core);             // 放tcpip核心锁
}

#endif // LWIP_TCPIP_CORE_LOCKING                  // 核心锁模式结束

#if !NO_SYS                                        // 带OS才有线程句柄
static TaskHandle_t lwip_tcpip_thread;       // 保存tcpip内核线程的句柄
#endif                                             // 条件编译结束

// 函数：sys_mark_tcpip_thread —— 标记tcpip内核线程
// 功能：tcpip内核线程入口自己调一次：把当前线程ID存到全局变量，之后sys_check_core_locking靠它判断“谁在持锁”，LwIP用它验证内核一致性
// 参数：无
// 返回：void，无返回值
void // 返回类型void
sys_mark_tcpip_thread(void)                        // 标记tcpip线程
{
#if !NO_SYS                                        // 带OS才有
  lwip_tcpip_thread = xTaskGetCurrentTaskHandle();   // 记录当前任务句柄
#endif                                             // 条件编译结束
}

// 函数：sys_check_core_locking —— 核心锁持有检查
// 功能：LwIP调试保护：进内核函数时检查是不是持有核心锁的线程在操作——不是tcpip线程又没拿锁就触发断言拦截，能在开发期就揪出“绕过锁偷改内核”的隐藏并发bug
// 参数：无
// 返回：void，无返回值，违规时断言挂起系统方便查
void // 返回类型void
sys_check_core_locking(void)                       // 核心锁检查
{
  // 先进出一次临界区，中断里调会失败，间接证明不在中断上下文
  taskENTER_CRITICAL();                             // 进临界区
  taskEXIT_CRITICAL();                              // 出临界区

#if !NO_SYS                                        // 带OS才检查
  if (lwip_tcpip_thread != 0) {                    // tcpip线程已标记
    TaskHandle_t current_thread = xTaskGetCurrentTaskHandle();  // 拿当前任务句柄

#if LWIP_TCPIP_CORE_LOCKING                        // 开核心锁模式
    // 开核心锁的话，必须持有锁
    LWIP_ASSERT("Function called without core lock",   // 断言文本
                current_thread == lwip_core_lock_holder_thread && lwip_core_lock_count > 0);   // 必须持锁
#else                                              // 没开核心锁
    // 没开核心锁，必须是tcpip线程自己
    LWIP_ASSERT("Function called from wrong thread", current_thread == lwip_tcpip_thread);   // 必须是内核线程
#endif                                             // 内层条件编译结束
  }
#endif // !NO_SYS 带OS条件编译结束
}
#endif // LWIP_FREERTOS_CHECK_CORE_LOCKING 条件编译结束
