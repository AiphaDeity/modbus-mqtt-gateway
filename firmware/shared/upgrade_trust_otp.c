#include "upgrade_trust.h"

#include "stm32f4xx.h"
#include "mbedtls/ecp.h"

#include <stddef.h>
#include <string.h>

// F407的OTP块12～15保存一份128 B记录，对应的四个锁字节须均为0。
#define TRUST_OTP_FIRST_BLOCK 12U
#define TRUST_OTP_BLOCK_COUNT 4U
#define TRUST_OTP_BLOCK_SIZE 32U
#define TRUST_OTP_LOCK_OFFSET 512U
#define TRUST_RECORD_SIZE 128U
#define TRUST_PUBLIC_KEY_OFFSET 20U
#define TRUST_AES_KEY_OFFSET 85U
#define TRUST_RESERVED_OFFSET 117U
#define TRUST_CRC_OFFSET 124U

static const uint8_t trust_magic[8] = {'G', 'W', 'T', 'R', 'U', 'S', 'T', 0U};

_Static_assert(TRUST_OTP_BLOCK_COUNT * TRUST_OTP_BLOCK_SIZE == TRUST_RECORD_SIZE,
               "Trust record must occupy four OTP blocks");
_Static_assert(FLASH_OTP_BASE + TRUST_OTP_LOCK_OFFSET + TRUST_OTP_FIRST_BLOCK
                 + TRUST_OTP_BLOCK_COUNT - 1U <= FLASH_OTP_END,
               "Trust locks outside OTP");

// 记录整数采用小端编码，不依赖结构体布局或指针对齐。
static uint32_t Read32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8U)
       | ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

// 检查空白、零值密钥及保留字段，不输出记录内容。
static bool AllValue(const uint8_t *data, uint32_t size, uint8_t value)
{
  for (uint32_t i = 0U; i < size; i++)
  {
    if (data[i] != value)
    {
      return false;
    }
  }
  return true;
}

// CRC-32/ISO-HDLC覆盖前124 B，仅检查意外损坏，不替代写入来源的可信性。
static uint32_t RecordCrc(const uint8_t *data)
{
  uint32_t crc = UINT32_MAX;
  for (uint32_t i = 0U; i < TRUST_CRC_OFFSET; i++)
  {
    crc ^= data[i];
    for (uint32_t bit = 0U; bit < 8U; bit++)
    {
      crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
    }
  }
  return ~crc;
}

// 复用现有密码库检查SEC1未压缩P-256点，拒绝仅前缀正确的无效公钥。
static bool PublicKeyValid(const uint8_t *public_key)
{
  if (public_key[0] != 0x04U)
  {
    return false;
  }
  mbedtls_ecp_group group;
  mbedtls_ecp_point point;
  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&point);
  bool valid = (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0)
            && (mbedtls_ecp_point_read_binary(&group, &point, public_key, 65U) == 0)
            && (mbedtls_ecp_check_pubkey(&group, &point) == 0);
  mbedtls_ecp_point_free(&point);
  mbedtls_ecp_group_free(&group);
  return valid;
}

// 启动时只读已锁定的OTP记录；失败清零输出，成功指针直接指向不可变OTP。
bool UpgradeTrust_ReadPlatform(UpgradeTrustKeys *keys)
{
  if (keys == NULL)
  {
    return false;
  }
  memset(keys, 0, sizeof(*keys));
  const volatile uint8_t *locks = (const volatile uint8_t *)(FLASH_OTP_BASE
                               + TRUST_OTP_LOCK_OFFSET + TRUST_OTP_FIRST_BLOCK);
  for (uint32_t i = 0U; i < TRUST_OTP_BLOCK_COUNT; i++)
  {
    if (locks[i] != 0U)
    {
      return false;
    }
  }
  const uint8_t *record = (const uint8_t *)(FLASH_OTP_BASE
                       + TRUST_OTP_FIRST_BLOCK * TRUST_OTP_BLOCK_SIZE);
  if ((memcmp(record, trust_magic, sizeof(trust_magic)) != 0)
      || (Read32(record + 8U) != TRUST_RECORD_SIZE)
      || (Read32(record + 12U) != 1U) || (Read32(record + 16U) != 1U)
      || !AllValue(record + TRUST_RESERVED_OFFSET,
                    TRUST_CRC_OFFSET - TRUST_RESERVED_OFFSET, 0U)
      || (Read32(record + TRUST_CRC_OFFSET) != RecordCrc(record)))
  {
    return false;
  }
  const uint8_t *aes_key = record + TRUST_AES_KEY_OFFSET;
  if (AllValue(aes_key, 32U, 0U) || AllValue(aes_key, 32U, 0xFFU)
      || !PublicKeyValid(record + TRUST_PUBLIC_KEY_OFFSET))
  {
    return false;
  }
  keys->signing_key_id = 1U;
  keys->public_key = record + TRUST_PUBLIC_KEY_OFFSET;
  keys->decrypt_key_id = 1U;
  keys->aes_key = aes_key;
  return true;
}
