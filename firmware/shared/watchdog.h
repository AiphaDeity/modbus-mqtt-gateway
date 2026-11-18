#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>

// 只读检查选项字节；本方案要求软件启动模式，不修改选项字节。
bool Watchdog_IsSoftwareMode(void);

/*
 * 普通上下文启动或接管 IWDG；HAL 毫秒时基须已初始化并正常前进。
 * 使用256分频及4095重装值，超时随LSI变化，不能当作精确32秒计时器。
 * freeze_on_debug仅决定CPU被调试器暂停时是否冻结计数。
 * 成功返回true；失败时IWDG可能已启动，调用方停止启动且禁止继续喂狗。
 * Boot/App各自在本镜像初始化时调用一次，无动态内存，不关闭中断。
 * App 接管时仍使用已计数的同一个 IWDG，Boot/App 交接过程不关闭硬件监督。
 */
bool Watchdog_Start(bool freeze_on_debug);

// 仅执行硬件重装，由当前监督所有者在检查通过后调用；不能放到滴答中断、空闲钩子或无条件循环。
void Watchdog_Refresh(void);

#endif
