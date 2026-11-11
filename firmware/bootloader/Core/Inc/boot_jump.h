#ifndef BOOT_JUMP_H
#define BOOT_JUMP_H 

#include <stdint.h>
#include <stdbool.h>

// 清理 Boot 硬件后跳转 App，成功不返回；失败可能已关闭部分硬件。
bool Boot_JumpToApp(void);

// 检查 MSP 范围/对齐及 Thumb 入口地址；通过不表示固件已认证。
bool Boot_IsAppVectorValid(uint32_t initial_sp, uint32_t reset_vector);

// 读取并检查候选应用的向量表；通过返回 true，否则返回 false
bool Boot_CheckAppVector(void);

#endif
