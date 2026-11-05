#ifndef M3_SELFTEST_H
#define M3_SELFTEST_H

/*
 * M3 上板自检。
 *
 * 上电后在 Storage_Task 里跑一次，把 SPI Flash / RTC / TF 卡 / 身份 /
 * 序列号 / 时间下界 / 控制日志的状态打印到日志串口（USART1, 115200 8N1）。
 *
 * 只做只读检查，不写任何持久数据（控制日志和 TF 卡记录除外——它们本身
 * 就是只读遍历）。量产时把 M3_SELFTEST_ENABLE 置 0 即可完全移除。
 */
#ifndef M3_SELFTEST_ENABLE
#define M3_SELFTEST_ENABLE 1
#endif

#if M3_SELFTEST_ENABLE
void m3_selftest_run(void);
#endif

#endif /* M3_SELFTEST_H */
