#include "upgrade_trust.h"

#include <stddef.h>

// 只保存平台置备资料的只读映射，不复制密钥到升级包或配置文件。
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

// Boot main 和 App 启动装配取得平台信任资料时调用，读取并校验映射后才允许后续按编号查询。
bool UpgradeTrust_Initialize(void)
{
  // 1. 本次运行已经绑定则复用映射，不重新读取或改变可信密钥。
  if (bound)
  {
    return true;
  }
  // 2. 局部结构体只接收编号和只读指针，实际密钥仍保留在平台置备区。
  UpgradeTrustKeys keys = {0};
  // 3. ReadPlatform 在当前工程校验 Boot 预留记录，成功后 Bind 保存映射；任一步失败都返回 false。
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
  // 结构体复制的是指针，底层存储及所指资料须在整个运行期间保持有效且不变。
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
  // 初始化未成功或包要求的编号不匹配时，输出保持空指针，由调用方关闭本次升级处理。
  if (!bound || (trusted.signing_key_id != signing_key_id) || (trusted.decrypt_key_id != decrypt_key_id))
  {
    return false;
  }
  // 成功只交付只读指针；这里不生成密钥，也不把密钥内容写进日志或升级包。
  *public_key = trusted.public_key;
  *aes_key = trusted.aes_key;
  return true;
}
