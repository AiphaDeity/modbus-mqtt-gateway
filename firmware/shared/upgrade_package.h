#ifndef UPGRADE_PACKAGE_H
#define UPGRADE_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "upgrade_baseline.h"

#define UPGRADE_PACKAGE_MANIFEST_SIZE 128U
#define UPGRADE_PACKAGE_OVERHEAD 208U
#define UPGRADE_PACKAGE_APP_ADDRESS 0x08010000U
#define UPGRADE_PACKAGE_APP_CAPACITY (448U * 1024U)
#define UPGRADE_PACKAGE_CAP_IWDG_HANDOFF 0x0001U
#define UPGRADE_PACKAGE_DECRYPT_KEY_ID_OFFSET 44U
#define UPGRADE_PACKAGE_SIGNING_KEY_ID_OFFSET 108U
#define UPGRADE_PACKAGE_TIMEOUT_MS 180000U
#define UPGRADE_PACKAGE_SIGNATURE_TIMEOUT_MS 10000U

typedef enum
{
  UPGRADE_PACKAGE_ERROR_NONE,
  UPGRADE_PACKAGE_ERROR_INVALID,
  UPGRADE_PACKAGE_ERROR_IO,
  UPGRADE_PACKAGE_ERROR_AUTH,
  UPGRADE_PACKAGE_ERROR_INTEGRITY,
  UPGRADE_PACKAGE_ERROR_IMAGE,
  UPGRADE_PACKAGE_ERROR_TIMEOUT,
  UPGRADE_PACKAGE_ERROR_INTERNAL
} UpgradePackageError;

// 验签阶段事件，供平台统计分段耗时。
typedef enum
{
  UPGRADE_PACKAGE_SIGNATURE_BEGIN,
  UPGRADE_PACKAGE_SIGNATURE_STEP_BEGIN,
  UPGRADE_PACKAGE_SIGNATURE_STEP_END,
  UPGRADE_PACKAGE_SIGNATURE_END
} UpgradePackageSignatureEvent;

/*
 * 同步读取/输出；offset、size单位为B，完整处理才返回true。
 * 缓冲只在调用期间有效，返回前结束所有后台访问，不能保留指针。
 * 调用方独占同一不可变源包；存储回调自身须有界，失败不自动重试。
 */
typedef bool (*UpgradePackageRead)(void *context, size_t offset, uint8_t *data, size_t size);
typedef bool (*UpgradePackageWrite)(void *context, size_t offset, const uint8_t *data, size_t size);

/*
 * 平台只提供单调毫秒时基、期限检查及实际进展处理，不包含存储或密钥。
 * checkpoint在progress=false时不得喂狗；true仅用于已完成的真实计算/分块。
 * 期限到达时仍调用checkpoint，使Boot能锁存监督失败；公共层也独立检查时限。
 * signature_event可为NULL；其他回调必须提供，回调与context在整次调用内有效。
 */
typedef struct
{
  void *context;
  uint32_t (*now_ms)(void *context);
  bool (*checkpoint)(void *context, uint32_t started_ms, uint32_t timeout_ms, bool progress);
  void (*signature_event)(void *context, UpgradePackageSignatureEvent event, int result);
} UpgradePackagePlatform;

/*
 * 调用方在进入公开操作前创建，started_ms只取一次，timeout_ms为1～0x7FFFFFFF。
 * App须沿用Finish的原起点及总期限；多遍认证、解密与验签不得重置起点。
 * error初始化为NONE，失败只记录首因；结构在整次同步调用期间独占。
 */
typedef struct
{
  const UpgradePackagePlatform *platform;
  uint32_t started_ms;
  uint32_t timeout_ms;
  UpgradePackageError error;
} UpgradePackageOperation;

typedef struct
{
  uint32_t firmware_size;
  uint16_t capabilities;
  uint8_t digest[32];
} UpgradePackageImage;

// 纯格式检查，失败输出清零；成功尚未认证，不能据此取得安装资格。
bool UpgradePackage_CheckManifest(const uint8_t *manifest, size_t manifest_size,
                                  size_t package_size, uint32_t *firmware_size,
                                  uint16_t *capabilities);

/*
 * 以下认证操作使用 Mbed TLS 动态工作区，须串行调用。
 * 分段ECDSA临时设置库全局运算预算，结束恢复默认；不可与其他ECP操作并发。
 * 公钥65 B、AES密钥32 B均由可信映射提供，不能取自待验包；调用期间不变。
 * VerifyDigestResult返回0或原库错误码，digest/signature分别可读32/64 B。
 */
int UpgradePackage_VerifyDigestResult(UpgradePackageOperation *operation,
                                      const uint8_t *digest, const uint8_t *signature,
                                      const uint8_t *public_key);
bool UpgradePackage_VerifySignature(UpgradePackageOperation *operation,
                                    UpgradePackageRead read, void *context, size_t package_size,
                                    const uint8_t *public_key, uint32_t *firmware_size);
// 完整认证签名、GCM 及明文摘要；向量检查由 VerifyImage 完成。
bool UpgradePackage_Verify(UpgradePackageOperation *operation,
                           UpgradePackageRead read, void *context, size_t package_size,
                           const uint8_t *public_key, const uint8_t *aes_key,
                           uint32_t *firmware_size);

// 先完整认证，再第二遍解密输出并复核；失败时目标可能已有部分变化，明文不落外存。
bool UpgradePackage_Decrypt(UpgradePackageOperation *operation,
                            UpgradePackageRead read, void *read_context, size_t package_size,
                            const uint8_t *public_key, const uint8_t *aes_key,
                            UpgradePackageWrite write, void *write_context);

// 向量纯检查：普通SRAM邮箱之后、8 B栈对齐、Thumb入口在本包有效App范围内。
bool UpgradePackage_CheckVector(const uint8_t *vector, uint32_t firmware_size);
// 完整认证及向量成功后才输出可信元数据，失败image清零；不输出固件明文。
bool UpgradePackage_VerifyImage(UpgradePackageOperation *operation,
                                UpgradePackageRead read, void *context, size_t package_size,
                                const uint8_t *public_key, const uint8_t *aes_key,
                                UpgradePackageImage *image);

#endif
