#include "app_entry.h"
#include "app_modules.h"
#include "app_readiness.h"
#include "app_upgrade.h"
#include "app_monitor.h"
#include "upgrade_stage.h"
#include "upgrade_entry.h"
#include "upgrade_tf.h"
#include "upgrade_trust.h"
#include "cmsis_os2.h"
#include "main.h"
#if GATEWAY_ENABLE_M2
#include "ethernet.h"   /* m2_ethernet_init()：以太网/LwIP 初始化入口 */
#include "network_upgrade.h"
#endif
#include <stdio.h>      /* printf：网络初始化失败时记录最小错误信息（不阻塞业务） */

// 按来源查询本次操作权限，不将包字段或网络连接状态当作授权。
static bool AuthorizeUpgrade(void *context, const UpgradeEntryPeer *peer)
{
  (void)context;
  if ((peer == NULL) || (peer->epoch == 0U))
  {
    return false;
  }
  switch (peer->source)
  {
    case UPGRADE_STAGE_SOURCE_TF:
      return UpgradeTf_Authorize(peer);
#if GATEWAY_ENABLE_M2
    case UPGRADE_STAGE_SOURCE_NETWORK:
      return NetworkUpgrade_Authorize(peer);
#endif
    default:
      return false;
  }
}

/*
 * 调度器启动后完成就绪状态、模块资源和业务任务的装配，然后退出启动任务。
 * argument 未使用；运行期资源状态由所属模块报告。
 */
void App_Entry(void *argument)
{
  (void)argument;
  // 可选的保护区只读映射；未配置只关闭升级入口，不阻断普通降级业务启动。
  (void)UpgradeTrust_Initialize();
  // 在其他模块和网络线程启动前读取Boot消息；普通独立调试App不使用保留邮箱。
  App_UpgradeInit((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS,
                  SCB->VTOR == 0x08010000U, osKernelGetTickCount());
  if (!UpgradeStage_Init())
  {
    Error_Handler();
    return;
  }
  const UpgradeEntryPolicy policy = {AuthorizeUpgrade, NULL};
  if (UpgradeEntry_BindPolicy(&policy) != UPGRADE_STAGE_OK)
  {
    printf("[App] upgrade policy bind FAILED\r\n");
    Error_Handler();
    return;
  }
  // 监督先于耗时初始化启动，覆盖网络/业务初始化阻塞；不另建喂狗任务。
  App_MonitorStart();

#if GATEWAY_ENABLE_M2
  // 在任务中初始化网络；局部失败最多尝试五次，再继续装配本地业务。
  {
    int retry;
    int32_t network_status = ETH_ERR_CONFIG;
    for (retry = 0; retry < 5; retry++)
    {
      network_status = m2_ethernet_init();
      if (network_status == ETH_OK)
        break;
      printf("[App] m2_ethernet_init 失败(%d/5)\r\n", retry + 1);
      if (retry < 4)
      {
        osDelay(2000);
      }
    }
    if (network_status != ETH_OK)
    {
      printf("[App] m2_ethernet_init 最终失败：网络保持未就绪，继续装配其他模块\r\n");
    }
  }
#endif /* GATEWAY_ENABLE_M2 */

  if (!App_ReadinessInit())
  {
    printf("[App] ReadinessInit FAILED\r\n");
    Error_Handler();
    return;
  }
  printf("[App] ReadinessInit OK, starting ModulesInit\r\n");

  App_ModulesInit();
  printf("[App] ModulesInit done, starting ModulesStart\r\n");
  App_ModulesStart();
  printf("[App] ModulesStart done\r\n");
  Monitor_StartupComplete();
  AppMemory_Report("startup");

  // 启动任务完成一次装配后退出
  osThreadExit();
  Error_Handler();
}
