#ifndef UPGRADE_STAGE_MBEDTLS_CONFIG_H
#define UPGRADE_STAGE_MBEDTLS_CONFIG_H

// 与Boot使用同一组安全包算法；密钥由可信映射单独提供。
#define MBEDTLS_AES_ROM_TABLES // 固定查表存放于Flash，释放应用普通SRAM。
// ★统一 BLOCK_CIPHER_C：使 gcm_context=616B，与 https_tls_mbedtls_config.h 一致。
// 不加此宏会导致 upgrade 的 gcm_context=384B 而 TLS 的=616B，ABI 错配。
#define MBEDTLS_BLOCK_CIPHER_C
#include "upgrade_mbedtls_config.h"

#endif
