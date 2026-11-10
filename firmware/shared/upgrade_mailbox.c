#include "upgrade_mailbox.h"

#include <stddef.h>
#include <string.h>

#define MAILBOX_MAGIC 0x55504731U

// 计算邮箱正文的 CRC，排除有效标记和 CRC 字段。
static uint32_t MessageCrc(const uint32_t *words)
{
  uint32_t crc = UINT32_MAX;
  for (uint32_t i = 1U; i < UPGRADE_MAILBOX_WORDS - 1U; i++)
  {
    for (uint32_t byte = 0U; byte < 4U; byte++)
    {
      crc ^= (words[i] >> (byte * 8U)) & 0xFFU;
      for (uint32_t bit = 0U; bit < 8U; bit++)
      {
        crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
      }
    }
  }
  return ~crc;
}

// 读取并校验邮箱快照，成功后解码启动消息。
bool UpgradeMailbox_Read(const volatile uint32_t *buffer, UpgradeMessage *message)
{
  if (message == NULL)
  {
    return false;
  }
  memset(message, 0, sizeof(*message));
  if (buffer == NULL)
  {
    return false;
  }
  uint32_t words[UPGRADE_MAILBOX_WORDS];
  for (uint32_t i = 0U; i < UPGRADE_MAILBOX_WORDS; i++)
  {
    words[i] = buffer[i];
  }
  if ((words[0] != MAILBOX_MAGIC) || (buffer[0] != MAILBOX_MAGIC)
      || (words[1] != UPGRADE_PERSISTENCE_VERSION) || (words[2] < UPGRADE_MESSAGE_NORMAL)
      || (words[2] > UPGRADE_MESSAGE_RECOVERY_CONFIRM)
#if UPGRADE_PERSISTENCE_VERSION == 1
      || (words[12] != 0U) || (words[13] != 0U) || (words[14] != 0U)
#else
      || ((words[12] == 0U) && (words[13] == 0U)) || (words[3] == 0U)
      || (words[14] > UPGRADE_TRIAL_LIMIT)
      || ((words[2] != UPGRADE_MESSAGE_NORMAL) && (words[14] == 0U))
#endif
      || (words[15] != MessageCrc(words)))
  {
    return false;
  }
  message->kind = (UpgradeMessageKind)words[2];
  message->sequence = words[3];
  message->tx_id = (uint64_t)words[12] | ((uint64_t)words[13] << 32U);
  message->trial_attempts = words[14];
  for (uint32_t i = 0U; i < sizeof(message->digest); i++)
  {
    message->digest[i] = (uint8_t)(words[4U + i / 4U] >> (8U * (i % 4U)));
  }
  return true;
}

// 清除有效标记，使旧邮箱消息不可再被接收。
void UpgradeMailbox_Clear(volatile uint32_t *buffer)
{
  if (buffer != NULL)
  {
    buffer[0] = 0U;
  }
}

// 先使旧消息失效，再写正文和 CRC，最后发布有效标记。
void UpgradeMailbox_Write(volatile uint32_t *buffer, const UpgradeMessage *message)
{
  if ((buffer == NULL) || (message == NULL))
  {
    return;
  }
  uint32_t words[UPGRADE_MAILBOX_WORDS] = {0};
  words[1] = UPGRADE_PERSISTENCE_VERSION;
  words[2] = (uint32_t)message->kind;
  words[3] = message->sequence;
#if UPGRADE_PERSISTENCE_VERSION == 2
  words[12] = (uint32_t)message->tx_id;
  words[13] = (uint32_t)(message->tx_id >> 32U);
  words[14] = message->trial_attempts;
#endif
  for (uint32_t i = 0U; i < sizeof(message->digest); i++)
  {
    words[4U + i / 4U] |= (uint32_t)message->digest[i] << (8U * (i % 4U));
  }
  words[15] = MessageCrc(words);
  UpgradeMailbox_Clear(buffer);
  for (uint32_t i = 1U; i < UPGRADE_MAILBOX_WORDS; i++)
  {
    buffer[i] = words[i];
  }
  buffer[0] = MAILBOX_MAGIC;
}
