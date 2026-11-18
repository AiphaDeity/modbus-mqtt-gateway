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
 * 由 freertos.c 的 StartDefaultTask 在调度器启动后调用，装配完成后退出启动任务。
 * 主线为启动身份与权限、提前监督、网络、就绪协调、模块初始化、任务创建及退出。
 * argument 未使用；运行期资源状态由所属模块报告。
 */
void App_Entry(void *argument)
{
  (void)argument;
  // 1. 取得升级信任资料并消费 Boot 邮箱，先确定本次是正常启动还是试运行。
  // 保护区资料未配置时升级入口保持不可用，本函数仍继续装配本地业务。
  (void)UpgradeTrust_Initialize();
  // 在其他模块和网络线程启动前读取Boot消息；普通独立调试App不使用保留邮箱。
  App_UpgradeInit((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS,
                  SCB->VTOR == 0x08010000U, osKernelGetTickCount());
  // 2. 初始化接收状态机，再把 TF 按键及网络通知的来源检查绑定给统一升级入口。
  if (!UpgradeStage_Init())
  {
    Error_Handler();
    return;
  }
  const UpgradeEntryPolicy policy = {AuthorizeUpgrade, NULL};
  // 绑定时复制函数指针和上下文；后续升级请求检查来源权限时才回调 AuthorizeUpgrade。
  // 来源获准后仍须经过 Stage 的固件包认证，绑定成功不代表已接受某个升级包。
  if (UpgradeEntry_BindPolicy(&policy) != UPGRADE_STAGE_OK)
  {
    printf("[App] upgrade policy bind FAILED\r\n");
    Error_Handler();
    return;
  }
  // 3. 提前创建 Monitor，监督后续耗时初始化；它仍须等待全部必需模块自检才能确认试运行。
  App_MonitorStart();

#if GATEWAY_ENABLE_M2
  // 4. 在当前启动任务中初始化网络，最多五次；网络失败保持未就绪，继续装配本地业务。
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

  // 5. 建立资源就绪事件组；各模块稍后报告状态，任务创建成功本身不等于资源就绪。
  if (!App_ReadinessInit())
  {
    printf("[App] ReadinessInit FAILED\r\n");
    Error_Handler();
    return;
  }
  printf("[App] ReadinessInit OK, starting ModulesInit\r\n");

  // 6. Init 按 app_modules.c 的装配表初始化资源并登记结果，Start 检查结果后集中创建业务任务。
  App_ModulesInit();
  printf("[App] ModulesInit done, starting ModulesStart\r\n");
  App_ModulesStart();
  printf("[App] ModulesStart done\r\n");
  // Start 内部解锁调度后，业务任务已经可以运行；下面的通知不控制它们的首次执行。
  // 7. 通知 Monitor 装配结束；它随后按运行期规则监督任务，而非立即把产品标为健康。
  Monitor_StartupComplete();
  // 此处栈余量属于当前 defaultTask；各业务任务的栈余量由 Monitor 分别检查。
  AppMemory_Report("startup");

  // 8. 退出当前 defaultTask；存储、采集、网络、升级及监督任务继续由调度器运行。
  osThreadExit();
  Error_Handler();
}
