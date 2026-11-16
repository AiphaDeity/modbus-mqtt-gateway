/*
 * 单个升级任务在已连接的TCP上使用TLS1.2；证书、域名和有效期均须校验。
 */
#ifndef HTTPS_TLS_H
#define HTTPS_TLS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lwip/api.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#ifdef __cplusplus
extern "C" {
#endif

// 调用方静态持有且首次清零；仅同一升级任务串行使用，不在ISR中调用。
typedef struct
{
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt cacert;
  struct netconn *conn;
  struct netbuf *rx_buf;
  uint16_t rx_offset;
  uint32_t started_ms;
  uint32_t timeout_ms;
  bool active;
} https_tls_session_t;

typedef enum
{
  HTTPS_TLS_OK = 0,
  HTTPS_TLS_ERR_INIT,
  HTTPS_TLS_ERR_HANDSHAKE,
  HTTPS_TLS_ERR_SEND,
  HTTPS_TLS_ERR_RECV,
  HTTPS_TLS_ERR_CLOSED,
  HTTPS_TLS_ERR_TIMEOUT,
  HTTPS_TLS_ERR_INTERNAL,
} https_tls_result_t;

// 初始化全局硬件熵和DRBG，失败不开放HTTPS；须在外设和调度器初始化后调用。
https_tls_result_t HttpsTls_Init(void);

/*
 * 在已连接的TCP上校验证书并握手；CA数据须在Close返回前保持不变。
 * 合法会话接管conn；失败后同样调用Close，conn非NULL表示仍未释放。
 * timeout_ms为握手、后续收发共用的固定总期限（ms，1～INT32_MAX），不随进展续期。
 */
https_tls_result_t HttpsTls_Connect(https_tls_session_t *session,
                                  struct netconn *conn,
                                  const char *hostname,
                                  const uint8_t *ca_cert_der,
                                  size_t ca_cert_len,
                                  uint32_t timeout_ms);

// 完整发送len字节；输入返回前保持有效，成功返回len，失败返回负的HTTPS_TLS_ERR_*。
int HttpsTls_Send(https_tls_session_t *session, const uint8_t *data, size_t len);

// 最多接收max_len字节；正数为实际长度，0为EOF，失败返回负的HTTPS_TLS_ERR_*。
int HttpsTls_Recv(https_tls_session_t *session, uint8_t *buf, size_t max_len);

// 释放未消费的TCP缓冲和TLS对象；连接删除失败保留conn，调用方须稍后重试。
void HttpsTls_Close(https_tls_session_t *session);

#ifdef __cplusplus
}
#endif

#endif
