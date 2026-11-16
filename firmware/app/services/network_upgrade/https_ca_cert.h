/*
 * https_ca_cert.h -- 百度云 BOS HTTPS 根 CA 证书 (DER)
 *
 * 预置DigiCert Global Root G2，用于当前BOS服务器证书链。
 * HttpsTls_Connect 用它校验服务器证书链, 防中间人攻击.
 */
#ifndef HTTPS_CA_CERT_H
#define HTTPS_CA_CERT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// DER常量放Flash，长度以g_https_ca_cert_der_len为准。
extern const uint8_t  g_https_ca_cert_der[];
extern const size_t   g_https_ca_cert_der_len;

#ifdef __cplusplus
}
#endif

#endif /* HTTPS_CA_CERT_H */
