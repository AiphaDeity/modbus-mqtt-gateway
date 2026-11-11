#ifndef BOOT_PACKAGE_H
#define BOOT_PACKAGE_H

/*
 * 本模块认证接口在 Boot 主线程串行调用，HAL 时基须运行。
 * 公钥为可信 SEC1 未压缩 65 B，AES 密钥为 32 B；输入、密钥及 context 在调用期间有效。
 * 使用 Mbed TLS 动态工作区，返回前释放；源包在认证和安装期间须保持不变。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "upgrade_package.h"

// App 能接管 Boot 已启动的 IWDG；这是签名方的声明，不代表已验证运行健康。
#define BOOT_PACKAGE_CAP_IWDG_HANDOFF UPGRADE_PACKAGE_CAP_IWDG_HANDOFF

/*
 * 检查 V2 包头、目标及整包长度；manifest 须可读 128 B，长度单位 B。
 * firmware_size 必须可写，capabilities 可为 NULL；输出不得与输入重叠，失败清零。
 * 仅接受 IWDG_HANDOFF，拒绝未知能力及非零保留区；成功结果尚未经密码认证。
 */
bool Boot_PackageCheckManifest(const uint8_t *manifest,
                              size_t manifest_size,
                              size_t package_size,
                              uint32_t *firmware_size,
                              uint16_t *capabilities);

/*
 * 用 P-256 公钥验证给定 SHA-256 摘要的签名，成功返回 true。
 * 调用方保证 digest、signature、public_key 分别可读 32、64、65 字节，
 * 调用期间保持有效。签名为大端 r || s，公钥为 SEC1 未压缩格式。
 * 摘要应来自 manifest || ciphertext || tag，由调用方计算并绑定实际包。
 * 在普通上下文调用，内部使用 Mbed TLS 动态内存，返回前释放。
 * 软件总上限10000 ms，库按运算预算分段，实际进展后检查原期限并喂狗；不续期。
 */
bool Boot_PackageVerifyDigest(const uint8_t *digest,
                             const uint8_t *signature,
                             const uint8_t *public_key);

/*
 * 同一验签流程的诊断入口，输入/所有权约定同上；0成功，非零为库错误码。
 * 用于自测区分签名拒绝和内部错误；监督失败同时由Boot_WatchdogFailed标识。
 * Boot裸机独占调用，会临时设置ECP运算预算，结束恢复为库默认不分段模式。
 */
int Boot_PackageVerifyDigestResult(const uint8_t *digest,
                                   const uint8_t *signature,
                                   const uint8_t *public_key);

/*
 * 同步读取包内数据，offset 和 size 均以字节计、相对包起点。
 * 完整填满 data 才返回 true，短读或错误返回 false；内部等待必须有界。
 * data 仅在本次调用期间有效，不得保留指针或在返回后继续 DMA 写入。
 * context 由调用方持有，可为 NULL。读取实现负责底层存储范围检查。
 */
typedef bool (*Boot_PackageRead)(void *context, size_t offset,
                                 uint8_t *data, size_t size);

/*
 * 分块计算 manifest || ciphertext || tag 的 SHA-256 并验签，每次读取不超过 256 B。
 * package_size 为整包字节数；firmware_size 必须可写且不与输入重叠，失败置零。
 * 固定总期限 180 s，分块/验签取得进展后才喂狗，不续期。
 * true 仅表示格式和签名通过，尚未验证 GCM、明文或安装结果。
 */
bool Boot_PackageVerifySignature(Boot_PackageRead read, void *context,
                                 size_t package_size, const uint8_t *public_key,
                                 uint32_t *firmware_size);

/*
 * 检查包头、签名、GCM、明文长度及摘要；firmware_size 成功输出固件字节数，失败置零。
 * 所有检查共用 180 s 固定期限，超时锁存监督失败。
 * 明文只在 SRAM 工作区暂存，释放前清零；成功不表示已安装或可直接跳转。
 */
bool Boot_PackageVerify(Boot_PackageRead read,
                        void *context,
                        size_t package_size,
                        const uint8_t *public_key,
                        const uint8_t *aes_key,
                        uint32_t *firmware_size);

/*
 * 明文输出回调：offset 为固件内字节偏移，data 仅在同步调用期间有效。
 * 不能保存指针；完整处理才返回 true。失败后目标可能已有部分变化。
 */
typedef bool (*Boot_PackageWrite)(void *context, size_t offset,
                                  const uint8_t *data, size_t size);

/*
 * 先完整认证整包，再重新流式解密并输出，同时复核 GCM/明文摘要。
 * 整个调用期间独占且禁止修改源包；输出失败或复核失败均返回 false。
 * 明文不落外部存储，内部工作区释放前清零；不能仅凭回调成功启动应用。
 * 预认证及再次解密/输出共用180000 ms上限，回调耗时也计入，失败不再输出后续块。
 */
bool Boot_PackageDecrypt(Boot_PackageRead read, void *read_context,
                         size_t package_size, const uint8_t *public_key,
                         const uint8_t *aes_key, Boot_PackageWrite write,
                         void *write_context);

// 完整认证并检查固件向量；输出失败清零，读取/keys生命周期与Verify相同。
bool Boot_PackageVerifyImage(Boot_PackageRead read, void *context, size_t package_size,
                              const uint8_t *public_key, const uint8_t *aes_key,
                              UpgradePackageImage *image);

#endif
