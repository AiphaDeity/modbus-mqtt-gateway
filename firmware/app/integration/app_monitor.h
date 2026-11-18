#ifndef APP_MONITOR_H
#define APP_MONITOR_H

#include "app_modules.h"
#include <stdbool.h>

// 只建立监督任务自身状态；产品健康默认false，不自动把装配成功认作健康。
AppModuleInitResult Monitor_Init(void);
// 周期检查时基、任务进展及栈余量，决定喂狗、试运行确认或复位。
void Monitor_Task(void *argument);

// App_Entry在全部业务任务创建成功后报告一次；重复调用不延长启动期限，ISR不可调用。
void Monitor_StartupComplete(void);

/*
 * 任务上下文报告产品自检结果，失效时立即报告 false。
 * 属于可选的额外产品约束；未调用时仍由全部必需模块的自检决定产品就绪。
 * 一旦报告 false 就阻止健康确认，报告 true 也不能替代模块进展及自检检查。
 */
void Monitor_ReportProductHealth(bool healthy);

/*
 * 任务所有者登记进展间隔和单次等待上限，单位 ms，均须为 1～0x7FFFFFFF。
 * 未登记的关键模块不能通过试运行；登记后超时会锁存故障并停止喂狗。
 * 以下登记、进展和等待接口只更新监督状态，不直接刷新 IWDG；单次等待须另行显式声明。
 */
bool Monitor_RegisterHealth(AppModuleId module, uint32_t progress_ms, uint32_t max_wait_ms);
/*
 * 完成实际工作或恢复步骤后报告进展，并更新模块自检结果；仅任务上下文调用。
 * 合法离线降级可通过自检；已超时的模块不能靠迟到报告恢复喂狗资格。
 */
bool Monitor_ReportProgress(AppModuleId module, bool self_test_passed);
// 声明一次有界等待，timeout_ms 单位 ms，不得超过登记上限；等待中或已超期时不能借此续期。
bool Monitor_BeginWait(AppModuleId module, uint32_t timeout_ms);
// 任务上下文锁存不可恢复故障；普通断网、无卡等业务状态不通过此接口上报。
void Monitor_ReportFatal(AppModuleId module);

/*
 * TF/网络升级任务在实际I/O/运算进展后报告，阻塞前声明等待；其他任务返回true。
 * 等待使用现有操作剩余期限，另留1000 ms返回处理裕量，不延长操作本身期限。
 */
bool Monitor_UpgradeProgress(void);
bool Monitor_UpgradeWait(uint32_t timeout_ms);

#endif
