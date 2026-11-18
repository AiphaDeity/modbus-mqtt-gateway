#ifndef UPGRADE_MBEDTLS_CONFIG_H
#define UPGRADE_MBEDTLS_CONFIG_H

// Boot/App 共用的安全包算法配置，仅启用所需算法及 P-256 分段验签。
#define MBEDTLS_SHA256_C  // SHA-256 摘要算法
#define MBEDTLS_AES_C     // AES 对称加密算法
#define MBEDTLS_GCM_C     // GCM 认证加密模式
#define MBEDTLS_BIGNUM_C  // 大整数运算模块
#define MBEDTLS_ECP_C     // 椭圆曲线基础运算模块
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED  // 启用secp256r1曲线（NISP-256）
#define MBEDTLS_ECDSA_C                   // 椭圆曲线数字签名算法
#define MBEDTLS_ECP_RESTARTABLE           // 椭圆曲线运算可分段执行
#define MBEDTLS_ASN1_PARSE_C              // ASN.1 数据解析模块
#define MBEDTLS_ASN1_WRITE_C              // ASN.1 数据编码模块

#endif
