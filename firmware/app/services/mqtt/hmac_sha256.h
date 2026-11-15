#ifndef HMAC_SHA256_H
#define HMAC_SHA256_H

#include <stddef.h>
#include <stdint.h>

/*
 * 轻量 HMAC-SHA256 实现（不依赖 mbedTLS，纯 C 约 3KB Flash）。
 *
 * 用途：云平台 MQTT 接入签名。
 *   - 百度云 IoT Core：password = lowercase_hex( SHA256(deviceKey&ts&SHA256deviceSecret) )
 *   - OneNET/阿里云：HMAC-SHA256 变体（见各平台算法）
 *
 * sha256_hex()：纯 SHA256（非 HMAC），百度云签名用。
 * hmac_sha256() / hmac_sha256_hex()：HMAC-SHA256，其他平台用。
 */

#define SHA256_DIGEST_SIZE   32
#define HMAC_SHA256_HEX_LEN  64   /* 32 字节 digest → 64 hex 字符 */

/* 计算 HMAC-SHA256，out 为 32 字节 digest */
void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len,
                 uint8_t out[SHA256_DIGEST_SIZE]);

/* HMAC-SHA256 输出转小写 hex 字符串（out_hex 至少 65 字节，含 '\0'） */
void hmac_sha256_hex(const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len,
                     char out_hex[HMAC_SHA256_HEX_LEN + 1]);

/* 纯 SHA256（非 HMAC），输出小写 hex 字符串（out_hex 至少 65 字节，含 '\0'）。
 * 百度云 IoT Core 签名用：password = SHA256(signStr) 的 hex。 */
void sha256_hex(const uint8_t *msg, size_t msg_len,
                char out_hex[HMAC_SHA256_HEX_LEN + 1]);

#endif /* HMAC_SHA256_H */
