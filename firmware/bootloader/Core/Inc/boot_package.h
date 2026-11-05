#ifndef BOOT_PACKAGE_H
#define BOOT_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 检查完整 V1 包头和整包长度。所有长度单位为字节，由普通上下文调用。
 * manifest 指向 manifest_size 字节的可读缓冲，当前要求恰好为 128 字节。
 * firmware_size 必须可写且与输入缓冲不重叠，失败置零，成功输出固件长度。
 * 返回 true 表示格式与目标检查通过，调用方仍须完成签名和 GCM 认证。
 */
bool Boot_PackageCheckManifest(const uint8_t *manifest,
                              size_t manifest_size,
                              size_t package_size,
                              uint32_t *firmware_size);

/*
 * 用 P-256 公钥验证给定 SHA-256 摘要的签名，成功返回 true。
 * 调用方保证 digest、signature、public_key 分别可读 32、64、65 字节，
 * 调用期间保持有效。签名为大端 r || s，公钥为 SEC1 未压缩格式。
 * 摘要应来自 manifest || ciphertext || tag，由调用方计算并绑定实际包。
 * 在普通上下文调用，内部使用 Mbed TLS 动态内存，返回前释放。
 */
bool Boot_PackageVerifyDigest(const uint8_t *digest,
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
 * 检查 V1 包头并分块计算 manifest || ciphertext || tag 的 SHA-256 后验签。
 * package_size 为实际整包字节数；read 每次最多读取 256 字节。
 * public_key 为调用方信任的 65 字节 P-256 公钥，不能从待验包中取得。
 * firmware_size 必须可写，失败置零，验签成功后输出固件字节数。
 * 输入、公钥和回调上下文在整个调用期间有效，输出不得与它们重叠。
 * 调用方须保证读取期间包内容不变，后续解密和安装仍须校验同一份包。
 * 在普通上下文同步调用，使用固定局部缓冲及 Mbed TLS 动态内存。
 * true 仅表示格式和签名通过，尚未进行 GCM 认证、明文校验或安装。
 */
bool Boot_PackageVerifySignature(Boot_PackageRead read, void *context,
                                 size_t package_size, const uint8_t *public_key,
                                 uint32_t *firmware_size);

/*
 * 完成 V1 包头、签名、GCM 标签、明文长度和 SHA-256 检查。
 * 沿用 VerifySignature 的读取、生命周期和可信公钥约定；aes_key 可读 32 字节，
 * 必须由调用方按已支持的密钥编号提供。包在验签、解密及后续安装期间须保持不变。
 * 普通上下文同步调用，内部动态分配有界工作区，分配失败返回 false。
 * 明文仅在内部 SRAM 工作区短暂出现，返回前清零，不输出、不落外部存储。
 * firmware_size 成功输出固件字节数，失败置零；true 不表示已安装或可直接跳转。
 */
bool Boot_PackageVerify(Boot_PackageRead read,
                        void *context,
                        size_t package_size,
                        const uint8_t *public_key,
                        const uint8_t *aes_key,
                        uint32_t *firmware_size);

#endif
