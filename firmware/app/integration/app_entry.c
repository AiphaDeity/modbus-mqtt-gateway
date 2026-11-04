#include "app_entry.h"
#include "app_modules.h"
#include "app_readiness.h"
#include "cmsis_os2.h"
#include "main.h"
#include "ethernet.h"   /* m2_ethernet_init()：以太网/LwIP 初始化入口 */


/*
 * 默认启动任务的应用入口，在调度器运行后执行一次，argument 当前不使用。
 * 先建立全未就绪的资源状态，再准备模块基础资源并集中创建业务任务。
 * 完成装配后退出；各模块任务上报就绪或失效，并按资源条件控制业务运行。
 */
void App_Entry(void *argument)
{
  (void)argument;

  /* ★网卡初始化必须在【任务上下文】里做，不能放回 main.c 的 osKernelStart 之前：
     m2_ethernet_init 内部的 taskENTER_CRITICAL()/taskEXIT_CRITICAL() 在调度器启动前
     会因 uxCriticalNesting 哨兵值(0xAAAAAAAA)配不平，把 BASEPRI 永久锁在 0x50，
     导致 TIM6 时基中断和 ETH 接收中断全被屏蔽（PING 不通的根因）。
     此时调度器已在跑，uxCriticalNesting=0，临界区能正确恢复 BASEPRI。 */
  if (m2_ethernet_init() != ETH_OK)
  {
    /* PHY 不通 / 协议栈内存不足：进错误处理（串口前面已有打印可定位是哪一类） */
    Error_Handler();
    return;
  }

  if (!App_ReadinessInit())
  {
    Error_Handler();
    return;
  }

  App_ModulesInit();
  App_ModulesStart();

  // 启动任务完成一次装配后退出
  osThreadExit();
  Error_Handler();
}
