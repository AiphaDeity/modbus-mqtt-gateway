#ifndef UPGRADE_MAILBOX_H
#define UPGRADE_MAILBOX_H

/*
 * 邮箱为独占的 16 个 volatile uint32_t，调用期间有效，接口不保留指针。
 * CRC 仅检测意外损坏；邮箱不保证掉电保持，最终确认以 Boot 的 Flash 状态为准。
 */

#include <stdbool.h>
#include <stdint.h>
#include "upgrade_baseline.h"

// Boot/偏移App链接脚本共同保留普通SRAM首64B，启动代码不清零此区域。
#define UPGRADE_MAILBOX_ADDRESS 0x20000000U
#define UPGRADE_MAILBOX_WORDS 16U

typedef enum
{
  UPGRADE_MESSAGE_NORMAL = 1,
  UPGRADE_MESSAGE_TRIAL,
  UPGRADE_MESSAGE_CONFIRM,
  UPGRADE_MESSAGE_RECOVERY_TRIAL,
  UPGRADE_MESSAGE_RECOVERY_CONFIRM
} UpgradeMessageKind;

typedef struct
{
  UpgradeMessageKind kind;
  uint32_t sequence;
  uint8_t digest[32];
  uint64_t tx_id;
  uint32_t trial_attempts;
} UpgradeMessage;

// 读取并校验消息，失败时清零输出。
bool UpgradeMailbox_Read(const volatile uint32_t *buffer, UpgradeMessage *message);
// 先使旧消息失效，写完正文和 CRC 后再发布有效标记。
void UpgradeMailbox_Write(volatile uint32_t *buffer, const UpgradeMessage *message);
// 清除有效标记，消费旧消息而不清空整个缓冲。
void UpgradeMailbox_Clear(volatile uint32_t *buffer);

#endif
