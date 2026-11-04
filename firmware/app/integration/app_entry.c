#include "app_entry.h"
#include "app_modules.h"
#include "app_readiness.h"
#include "cmsis_os2.h"
#include "main.h"
#include "ethernet.h"   /* m2_ethernet_init()：以太网/LwIP 初始化入口 */
#include <stdio.h>      /* printf：网络初始化失败时记录最小错误信息（不阻塞业务） */


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
     此时调度器已在跑，uxCriticalNesting=0，临界区能正确恢复 BASEPRI。
     ★评审要求：网络局部故障不能升级成整机停机——PHY/网线/DHCP 异常时，
     Modbus 和本地存储必须照常工作。这里最多重试 5 次，仍失败就打日志后继续装配，
     网络保持未就绪，后续由 MQTT 任务按策略重试，不再 Error_Handler。 */
  {
    int retry;
    for (retry = 0; retry < 5; retry++)
    {
      if (m2_ethernet_init() == ETH_OK)
        break;
      printf("[App] m2_ethernet_init 失败(%d/5)，2s 后重试\r\n", retry + 1);
      osDelay(2000);
    }
    if (m2_ethernet_init() != ETH_OK)
    {
      /* 幂等：g_m2_eth_inited 仍为 0，eth_link/eth_dhcp 线程未创建。
         记录一行最小错误信息后继续——串口日志是当前唯一诊断手段。 */
      printf("[App] m2_ethernet_init 最终失败：网络保持未就绪，继续装配其他模块\r\n");
    }
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
