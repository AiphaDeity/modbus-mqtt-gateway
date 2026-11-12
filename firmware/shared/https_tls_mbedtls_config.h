/*
 * https_tls_mbedtls_config.h —— HTTPS 固件下载 TLS 客户端配置（v3，集成入 App 构建）
 *
 * 目标服务器：百度云 BOS bj.bcebos.com:443
 *   TLS1.2 / ECDHE-RSA-AES128-GCM-SHA256 / 单向服务器认证 / RSA-2048 / 必须 SNI
 *
 * ★统一原则★：本配置 include upgrade_mbedtls_config.h（升级验签 10 宏），
 *   并显式启用 MBEDTLS_BLOCK_CIPHER_C，使 gcm_context 与升级算法同尺寸（实测 616B）。
 *   升级验签与 TLS 必须共用本配置编译，禁止两套配置混链（会导致 gcm_context 616 vs 384 错配）。
 *
 * v3 集成修正：
 *   - IN/OUT_CONTENT_LEN 改 2048（候选2，匹配可用 C 堆 ~11KiB）
 *   - 加 MBEDTLS_NO_PLATFORM_ENTROPY（裸机必须，否则 entropy_poll.c 编译失败）
 *   - 删 MBEDTLS_SSL_DEFAULT_CIPHERSUITES（3.6.7 无此宏，靠只启用 ECDHE_RSA 套件自然限制）
 *   - 加 MBEDTLS_BLOCK_CIPHER_C（统一 gcm_context 尺寸）
 *   - 加 MBEDTLS_HAVE_TIME/HAVE_TIME_DATE/PLATFORM_TIME（证书有效期检查）
 *   - 加 MBEDTLS_ECDH_C（ECDHE_RSA 前置依赖）
 *   - 加 MBEDTLS_ALLOW_PRIVATE_ACCESS（3.6 字段私有化后 sizeof 提取需要）
 */
#ifndef HTTPS_TLS_MBEDTLS_CONFIG_H
#define HTTPS_TLS_MBEDTLS_CONFIG_H

/* mbedTLS 3.6 字段私有化后，sizeof 提取需访问 private_ 成员别名 */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

/* ===== 1. 复用升级验签子集 + 统一 BLOCK_CIPHER ===== */
#define MBEDTLS_AES_ROM_TABLES          /* AES 查表放 Flash，省普通 SRAM */
#include "upgrade_mbedtls_config.h"     /* SHA256/AES/GCM/BIGNUM/ECP/ECDSA/ASN1 等 10 宏 */
#define MBEDTLS_BLOCK_CIPHER_C          /* ★统一：使 gcm_context=616B（与升级算法一致），不可删 */

/* ===== 2. TLS 协议栈（只用 TLS1.2 客户端）===== */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION  /* SNI：BCE 按域名分发证书，必须 */
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET  /* 安全性：extended master secret */

/* ===== 3. X.509 证书解析与验证 ===== */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE
#define MBEDTLS_X509_RSASSL_PSS_SUPPORT

/* ===== 4. 证书有效期检查（时间）===== */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_ALT       /* app 实现 mbedtls_platform_time(秒,RTC) */
#define MBEDTLS_PLATFORM_MS_TIME_ALT    /* app 实现 mbedtls_ms_time(毫秒,HAL_GetTick)；裸机必须，否则 #error */

/* ===== 5. PK / RSA ===== */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_RSA_C
#define MBEDTLS_PK_ECC_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15               /* 证书签名 RSASSA-PKCS1-v1_5 */

/* ===== 6. CIPHER ===== */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_GCM

/* ===== 7. 密钥交换：只启用 ECDHE-RSA（BCE 实测套件）===== */
#define MBEDTLS_ECDH_C                  /* ECDHE_RSA 前置（提供 MBEDTLS_CAN_ECDH）*/
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
/* 不启用其它 KEY_EXCHANGE_*，cipher suite 列表自然只剩 ECDHE-RSA-AES128-GCM-SHA256 */

/* ===== 8. 随机数（裸机）===== */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_NO_PLATFORM_ENTROPY     /* ★裸机必须：禁用 Unix/Windows 平台熵源，否则 entropy_poll.c 编译失败 */
#define MBEDTLS_ENTROPY_HARDWARE_ALT    /* 硬件熵源：app 需实现 mbedtls_hardware_poll(STM32F4 RNG 外设) */
#define MBEDTLS_ENTROPY_MAX_SOURCES     2   /* 只用硬件熵源，省 source[] 数组 */

/* ===== 9. PEM / BASE64 / MD / OID / PLATFORM ===== */
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PLATFORM_C
/* 不定义 MBEDTLS_PLATFORM_MEMORY：newlib calloc/free 经 _sbrk 已可用，免多余间接层 */

/* ===== 10. 内存相关宏（候选2：IN/OUT=2048，总峰值 ~8KiB）===== */
#define MBEDTLS_SSL_IN_CONTENT_LEN      2048   /* 候选2：每方向 2KiB，总 4KiB */
#define MBEDTLS_SSL_OUT_CONTENT_LEN     2048
/* #define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE */  /* 不保留对端证书：握手验证后释放，下载阶段省 ~6KiB */
/* #define MBEDTLS_ERROR_C */                    /* 关 error 字符串表，省 Flash；如需 strerror 再开 */

#endif /* HTTPS_TLS_MBEDTLS_CONFIG_H */
