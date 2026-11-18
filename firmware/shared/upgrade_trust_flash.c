#include "upgrade_trust.h"

#include "mbedtls/ecp.h"

#include <stddef.h>
#include <string.h>

// Boot链接脚本保留末尾128 B，普通App安装不会擦写此区域。
#define TRUST_FLASH_ADDRESS 0x0800FF80U
#define TRUST_RECORD_SIZE 128U
#define TRUST_PUBLIC_KEY_OFFSET 20U
#define TRUST_AES_KEY_OFFSET 85U
#define TRUST_RESERVED_OFFSET 117U
#define TRUST_CRC_OFFSET 124U

static const uint8_t trust_magic[8] = {'G', 'W', 'T', 'R', 'U', 'S', 'T', 0U};

_Static_assert(TRUST_FLASH_ADDRESS + TRUST_RECORD_SIZE == 0x08010000U,
               "Trust record must end at the App boundary");

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

// 启动时校验Boot预留记录；失败清零输出，成功返回运行期间有效的只读指针。
bool UpgradeTrust_ReadPlatform(UpgradeTrustKeys *keys)
{
  if (keys == NULL)
  {
    return false;
  }
  memset(keys, 0, sizeof(*keys));
  // App 和 Boot 都读取同一份预留记录；本函数只读，既不首次写入也不修复损坏资料。
  const uint8_t *record = (const uint8_t *)TRUST_FLASH_ADDRESS;
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
  // 格式、CRC 和密钥有效性全部检查通过后，才交出记录中的只读指针。
  keys->signing_key_id = 1U;
  keys->public_key = record + TRUST_PUBLIC_KEY_OFFSET;
  keys->decrypt_key_id = 1U;
  keys->aes_key = aes_key;
  return true;
}
