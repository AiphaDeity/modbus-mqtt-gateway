#ifndef UPGRADE_STAGE_MBEDTLS_CONFIG_H
#define UPGRADE_STAGE_MBEDTLS_CONFIG_H

// 与Boot使用同一组安全包算法；密钥由可信映射单独提供。
#define MBEDTLS_AES_ROM_TABLES // 固定查表存放于Flash，释放应用普通SRAM。
#include "upgrade_mbedtls_config.h"

#endif
