#include "upgrade_trust.h"

#include <stddef.h>

static UpgradeTrustKeys trusted;
static bool bound;

// 默认不提供密钥映射；由保护区适配实现覆盖此弱函数。
__attribute__((weak)) bool UpgradeTrust_ReadPlatform(UpgradeTrustKeys *keys)
{
  if (keys != NULL)
  {
    keys->public_key = NULL;
    keys->aes_key = NULL;
    keys->signing_key_id = 0U;
    keys->decrypt_key_id = 0U;
  }
  return false;
}

// 读取并绑定平台密钥映射，已有绑定时直接返回。
bool UpgradeTrust_Initialize(void)
{
  // 已绑定时直接返回
  if (bound)
  {
    return true;
  }
  // 准备一个局部结构体 keys
  UpgradeTrustKeys keys = {0};
  // 从 Boot 预留区读取并检查平台资料，读取成功，才会保存映射
  return UpgradeTrust_ReadPlatform(&keys) && UpgradeTrust_Bind(&keys);
}

// 当前包准入只识别编号1；明确拒绝映射错位，不默默接受任意包内编号。
bool UpgradeTrust_Bind(const UpgradeTrustKeys *keys)
{
  if (bound || (keys == NULL) || (keys->signing_key_id != 1U) || (keys->decrypt_key_id != 1U)
      || (keys->public_key == NULL) || (keys->aes_key == NULL) || (keys->public_key[0] != 0x04U))
  {
    return false;
  }
  trusted = *keys;
  bound = true;
  return true;
}

// 查询可信密钥映射是否已经绑定。
bool UpgradeTrust_IsReady(void)
{
  return bound;
}

// 匹配签名和解密密钥编号，返回对应只读指针。
bool UpgradeTrust_Get(uint32_t signing_key_id, uint32_t decrypt_key_id,
                       const uint8_t **public_key, const uint8_t **aes_key)
{
  if ((public_key == NULL) || (aes_key == NULL))
  {
    return false;
  }
  *public_key = NULL;
  *aes_key = NULL;
  if (!bound || (trusted.signing_key_id != signing_key_id) || (trusted.decrypt_key_id != decrypt_key_id))
  {
    return false;
  }
  *public_key = trusted.public_key;
  *aes_key = trusted.aes_key;
  return true;
}
