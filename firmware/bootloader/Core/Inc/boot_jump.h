#ifndef BOOT_JUMP_H
#define BOOT_JUMP_H 

#include <stdint.h>
#include <stdbool.h>

// 跳转到 APP
bool Boot_JumpToApp(void);

// 校验 APP 向量表是否合法，跳转前检查，防止跳飞、HardFault
bool Boot_IsAppVectorValid(uint32_t initial_sp, uint32_t reset_vector);

// 读取并检查候选应用的向量表；通过返回 true，否则返回 false
bool Boot_CheckAppVector(void);

#endif