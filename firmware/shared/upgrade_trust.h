#ifndef UPGRADE_TRUST_H
#define UPGRADE_TRUST_H

#include <stdbool.h>
#include <stdint.h>

// 调用方持有的只读可信映射；指针需在固件运行期间有效，不从包或配置文件取得。
typedef struct
{
  uint32_t signing_key_id;
  const uint8_t *public_key; // SEC1未压缩65 B。
  uint32_t decrypt_key_id;
  const uint8_t *aes_key;    // 32 B，不进入日志、TF或导出。
} UpgradeTrustKeys;

// 启动装配、所有升级入口开放前绑定一次；不可ISR/并发调用，不复制密钥内容。
bool UpgradeTrust_Bind(const UpgradeTrustKeys *keys);
// 默认平台实现返回false；生产适配从保护区只读提供映射，不把密钥编进通用源码。
bool UpgradeTrust_ReadPlatform(UpgradeTrustKeys *keys);
// 读取平台保护区映射并绑定；缺少映射或校验失败时返回 false。
bool UpgradeTrust_Initialize(void);
// 查询是否已绑定有效映射，不访问保护区。
bool UpgradeTrust_IsReady(void);
// 按密钥编号取得只读指针，失败时输出 NULL；返回的密钥在运行期间有效。
bool UpgradeTrust_Get(uint32_t signing_key_id, uint32_t decrypt_key_id,
                       const uint8_t **public_key, const uint8_t **aes_key);

#endif
