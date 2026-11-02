#include "app_entry.h"
#include "app_modules.h"
#include "app_readiness.h"
#include "cmsis_os2.h"
#include "main.h"


/*
 * 默认启动任务的应用入口，在调度器运行后执行一次，argument 当前不使用。
 * 先建立全未就绪的资源状态，再准备模块基础资源并集中创建业务任务。
 * 完成装配后退出；各模块任务上报就绪或失效，并按资源条件控制业务运行。
 */
void App_Entry(void *argument)
{
  (void)argument;

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
