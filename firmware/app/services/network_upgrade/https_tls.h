/*
 * https_tls.h —— HTTPS TLS 客户端包装器（基于 mbedTLS 3.6 + LwIP netconn）
 *
 * 在已有 TCP netconn 连接之上叠加 TLS1.2 加密层：
 *   HttpsTls_Init    → 一次性初始化（熵源、CA 证书、时间函数）
 *   HttpsTls_Connect → 在已建立的 TCP 连接上完成 TLS 握手
 *   HttpsTls_Send    → 加密发送（替代 netconn_write）
 *   HttpsTls_Recv    → 加密接收（替代 netconn_recv）
 *   HttpsTls_Close   → 关闭 TLS + TCP
 *
 * 下载器在 URL 以 https:// 开头时使用本模块，否则走原 HTTP netconn 路径。
 */
#ifndef HTTPS_TLS_H
#define HTTPS_TLS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "lwip/api.h"   /* struct netconn */
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== TLS 会话上下文 =====
 * 静态分配（放 CCMRAM），避免运行时 calloc 失败。
 * ssl_context 内部的 in/out_buffer 由 mbedTLS calloc（C 堆）分配。
 */
typedef struct
{
    mbedtls_ssl_context     ssl;        /* 212B */
    mbedtls_ssl_config      conf;       /* 108B */
    mbedtls_x509_crt        cacert;     /* 404B */
    mbedtls_entropy_context entropy;    /* 60B (MAX_SOURCES=2) */
    mbedtls_ctr_drbg_context ctr_drbg;  /* ~128B */
    struct netconn         *conn;       /* TCP 连接（由调用方创建并传入） */
    bool                    active;     /* TLS 握手已完成，可收发 */
} https_tls_session_t;

/* ===== 结果码 ===== */
typedef enum
{
    HTTPS_TLS_OK = 0,
    HTTPS_TLS_ERR_INIT,         /* 模块未初始化 */
    HTTPS_TLS_ERR_HANDSHAKE,    /* TLS 握手失败 */
    HTTPS_TLS_ERR_SEND,         /* 加密发送失败 */
    HTTPS_TLS_ERR_RECV,         /* 加密接收失败 */
    HTTPS_TLS_ERR_CLOSED,       /* 连接已关闭 */
    HTTPS_TLS_ERR_TIMEOUT,      /* 超时 */
    HTTPS_TLS_ERR_INTERNAL,     /* 内部错误 */
} https_tls_result_t;

/**
 * @brief  一次性初始化 TLS 模块（装配阶段调用）
 * @return HTTPS_TLS_OK=成功
 * @note   初始化全局熵源、CTR_DRBG 种子、注册时间函数。
 *         不加载 CA 证书（每次 Connect 时加载，支持热更换）。
 *         必须在 LwIP 和 RTC 初始化之后调用。
 */
https_tls_result_t HttpsTls_Init(void);

/**
 * @brief  在已建立的 TCP 连接上完成 TLS 握手
 * @param  session     TLS 会话（调用方分配，静态生命周期）
 * @param  conn        已 connect 成功的 TCP netconn（所有权转移到 session）
 * @param  hostname    服务器域名（SNI 必须填，证书 CN 校验也用它）
 * @param  ca_cert_der CA 证书 DER 数据（NULL=跳过验证，仅测试用）
 * @param  ca_cert_len CA 证书 DER 长度
 * @param  timeout_ms  握手超时
 * @return HTTPS_TLS_OK=握手成功，session->active=true
 */
https_tls_result_t HttpsTls_Connect(https_tls_session_t *session,
                                     struct netconn *conn,
                                     const char *hostname,
                                     const uint8_t *ca_cert_der,
                                     size_t ca_cert_len,
                                     uint32_t timeout_ms);

/**
 * @brief  加密发送（替代 netconn_write）
 * @return >0=已发送字节数，<0=错误
 */
int HttpsTls_Send(https_tls_session_t *session, const uint8_t *data, size_t len);

/**
 * @brief  加密接收（替代 netconn_recv）
 * @return >0=已接收字节数，0=EOF，<0=错误
 * @note   阻塞直到收到至少 1 字节或超时/错误。
 */
int HttpsTls_Recv(https_tls_session_t *session, uint8_t *buf, size_t max_len);

/**
 * @brief  关闭 TLS 会话并释放 TCP 连接
 * @note   幂等，可重复调用。
 */
void HttpsTls_Close(https_tls_session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* HTTPS_TLS_H */
