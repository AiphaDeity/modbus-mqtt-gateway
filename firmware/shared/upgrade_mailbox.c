#include "upgrade_mailbox.h"

#include <stddef.h>
#include <string.h>

/*
 * 普通 SRAM 首 64 B 保存 Boot/App 的复位交接消息，启动代码不得清零该保留区。
 * 邮箱不是持久日志；确认消息只有与复位原因、当前事务及实际镜像匹配后才可采信。
 */
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

// 将保留邮箱解码为消息，供 Boot 接收确认或 App 接收启动身份；格式及 CRC 检查通过才返回 true。
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
  // 先取得完整字快照，再检查它是否属于有效消息，失败时保持输出结构体为零。
  uint32_t words[UPGRADE_MAILBOX_WORDS];
  for (uint32_t i = 0U; i < UPGRADE_MAILBOX_WORDS; i++)
  {
    words[i] = buffer[i];
  }
  if ((words[0] != MAILBOX_MAGIC) || (buffer[0] != MAILBOX_MAGIC)
      || (words[1] != UPGRADE_PERSISTENCE_VERSION) || (words[2] < UPGRADE_MESSAGE_NORMAL)
      || (words[2] > UPGRADE_MESSAGE_RECOVERY_CONFIRM)
      || ((words[12] == 0U) && (words[13] == 0U)) || (words[3] == 0U)
      || (words[14] > UPGRADE_TRIAL_LIMIT)
      || ((words[2] != UPGRADE_MESSAGE_NORMAL) && (words[14] == 0U))
      || (words[15] != MessageCrc(words)))
  {
    return false;
  }
  // 按固定字布局还原身份字段；是否属于当前试运行，还由调用方与持久记录进一步比对。
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
  words[12] = (uint32_t)message->tx_id;
  words[13] = (uint32_t)(message->tx_id >> 32U);
  words[14] = message->trial_attempts;
  for (uint32_t i = 0U; i < sizeof(message->digest); i++)
  {
    words[4U + i / 4U] |= (uint32_t)message->digest[i] << (8U * (i % 4U));
  }
  words[15] = MessageCrc(words);
  // 先撤销旧有效标记；写到一半发生复位时，读取方不能把半份正文当成完整消息。
  UpgradeMailbox_Clear(buffer);
  for (uint32_t i = 1U; i < UPGRADE_MAILBOX_WORDS; i++)
  {
    buffer[i] = words[i];
  }
  // 正文和 CRC 写完后最后发布有效标记，调用方再按交接流程执行内存屏障或复位。
  buffer[0] = MAILBOX_MAGIC;
}
