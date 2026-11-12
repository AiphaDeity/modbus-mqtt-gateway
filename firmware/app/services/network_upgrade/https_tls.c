/*
 * https_tls.c —— HTTPS TLS 客户端包装器实现
 *
 * 在已有 TCP netconn 上叠加 TLS1.2（ECDHE-RSA-AES128-GCM-SHA256）。
 * mbedTLS 通过 bio 回调收发加密记录，bio 内部走 netconn_write/recv。
 */
#include "https_tls.h"

#include <string.h>
#include <stdio.h>

#include "lwip/api.h"
#include "lwip/netbuf.h"
#include "lwip/pbuf.h"

/* mbedTLS 错误码 */
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"   /* MBEDTLS_ERR_NET_* 错误码（仅头文件，不链接 net_sockets.c） */

/* 平台桩：注册时间函数（实现在 https_tls_stubs.c） */
extern void https_tls_register_time(void);

/* ===== 内部状态 ===== */
static bool s_module_initialized = false;
static mbedtls_entropy_context  s_entropy;
static mbedtls_ctr_drbg_context s_ctr_drbg;

/* ===== bio 回调：TCP 发送 =====
 * mbedTLS 加密后的 TLS 记录通过 netconn_write 发到 TCP 层。
 */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    https_tls_session_t *session = (https_tls_session_t *)ctx;
    if (!session || !session->conn) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    err_t err = netconn_write(session->conn, buf, len, NETCONN_COPY);
    if (err == ERR_OK)
    {
        return (int)len;
    }
    printf("[TLS] bio_send err=%d\r\n", (int)err);
    if (err == ERR_CLSD || err == ERR_CONN)
        return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

/* ===== bio 回调：TCP 接收 =====
 * mbedTLS 需要读 TLS 记录时调用此函数。
 * 从 netconn_recv 获取 netbuf → 提取数据到内部残余缓冲 → 喂给 mbedTLS。
 *
 * 残余缓冲：一次 netconn_recv 可能返回多条 TLS 记录，保留未消费部分。
 */
#define TLS_BIO_BUF_SIZE  2048   /* 与 TLS 记录最大长度匹配（IN_CONTENT_LEN=2048） */

typedef struct
{
    uint8_t  data[TLS_BIO_BUF_SIZE];
    uint16_t len;     /* 缓冲中剩余未消费字节数 */
    uint16_t offset;  /* 下一段待读起始偏移 */
} tls_bio_recv_buf_t;

static tls_bio_recv_buf_t s_bio_buf;  /* 单例：同一时刻只有一个 TLS 会话 */

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    https_tls_session_t *session = (https_tls_session_t *)ctx;
    if (!session || !session->conn) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    /* 有残余数据 → 先消费 */
    if (s_bio_buf.len > 0)
    {
        size_t give = (len < s_bio_buf.len) ? len : s_bio_buf.len;
        memcpy(buf, &s_bio_buf.data[s_bio_buf.offset], give);
        s_bio_buf.offset += (uint16_t)give;
        s_bio_buf.len    -= (uint16_t)give;
        if (s_bio_buf.len == 0) s_bio_buf.offset = 0;
        return (int)give;
    }

    /* 无残余 → 从 netconn 收新数据 */
    struct netbuf *nbuf = NULL;
    err_t err = netconn_recv(session->conn, &nbuf);
    if (err != ERR_OK)
    {
        if (err == ERR_CLSD || err == ERR_CONN)
            return MBEDTLS_ERR_NET_CONN_RESET;
        if (err == ERR_TIMEOUT)
            return MBEDTLS_ERR_SSL_TIMEOUT;
        printf("[TLS] bio_recv err=%d\r\n", (int)err);
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    /* 提取 netbuf 数据 */
    void *seg_data = NULL;
    uint16_t seg_len = 0;
    netbuf_data(nbuf, &seg_data, &seg_len);

    /* 一次性拷贝（seg_len <= TLS_BIO_BUF_SIZE 大概率成立） */
    uint16_t copy = seg_len;
    if (copy > TLS_BIO_BUF_SIZE) copy = TLS_BIO_BUF_SIZE;

    /* 如果 netbuf 有多段（pbuf 链），拼接 */
    s_bio_buf.len = 0;
    s_bio_buf.offset = 0;
    do {
        netbuf_data(nbuf, &seg_data, &seg_len);
        uint16_t space = TLS_BIO_BUF_SIZE - s_bio_buf.len;
        uint16_t chunk = (seg_len < space) ? seg_len : space;
        memcpy(&s_bio_buf.data[s_bio_buf.len], seg_data, chunk);
        s_bio_buf.len += chunk;
    } while (netbuf_next(nbuf) == ERR_OK);

    netbuf_delete(nbuf);

    /* 从缓冲中取请求长度 */
    size_t give = (len < s_bio_buf.len) ? len : s_bio_buf.len;
    memcpy(buf, &s_bio_buf.data[s_bio_buf.offset], give);
    s_bio_buf.offset += (uint16_t)give;
    s_bio_buf.len    -= (uint16_t)give;
    if (s_bio_buf.len == 0) s_bio_buf.offset = 0;
    return (int)give;
}

/* ===== 模块初始化 ===== */
https_tls_result_t HttpsTls_Init(void)
{
    if (s_module_initialized) return HTTPS_TLS_OK;

    /* 注册时间函数指针（MBEDTLS_PLATFORM_TIME_ALT 要求） */
    https_tls_register_time();

    /* 初始化熵源 */
    mbedtls_entropy_init(&s_entropy);

    /* 初始化 CTR_DRBG */
    mbedtls_ctr_drbg_init(&s_ctr_drbg);

    /* 播种 CTR_DRBG（用熵源 + 个性化字符串） */
    const char *pers = "https_ota_download";
    int ret = mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func,
                                     &s_entropy,
                                     (const unsigned char *)pers, strlen(pers));
    if (ret != 0)
    {
        printf("[TLS] ctr_drbg_seed fail: -0x%04x\r\n", (unsigned)-ret);
        mbedtls_entropy_free(&s_entropy);
        mbedtls_ctr_drbg_free(&s_ctr_drbg);
        return HTTPS_TLS_ERR_INIT;
    }

    s_module_initialized = true;
    printf("[TLS] module init OK\r\n");
    return HTTPS_TLS_OK;
}

/* ===== TLS 握手 ===== */
https_tls_result_t HttpsTls_Connect(https_tls_session_t *session,
                                     struct netconn *conn,
                                     const char *hostname,
                                     const uint8_t *ca_cert_der,
                                     size_t ca_cert_len,
                                     uint32_t timeout_ms)
{
    if (!s_module_initialized) return HTTPS_TLS_ERR_INIT;
    if (!session || !conn || !hostname) return HTTPS_TLS_ERR_INTERNAL;

    memset(session, 0, sizeof(*session));
    session->conn = conn;

    /* 重置 bio 残余缓冲 */
    s_bio_buf.len = 0;
    s_bio_buf.offset = 0;

    /* 初始化 SSL 上下文 */
    mbedtls_ssl_init(&session->ssl);
    mbedtls_ssl_config_init(&session->conf);
    mbedtls_x509_crt_init(&session->cacert);

    /* 加载 CA 证书（可选：NULL=跳过验证，仅测试用） */
    if (ca_cert_der && ca_cert_len > 0)
    {
        int ret = mbedtls_x509_crt_parse_der_nocopy(&session->cacert,
                                                      ca_cert_der, ca_cert_len);
        if (ret != 0)
        {
            printf("[TLS] CA cert parse fail: -0x%04x\r\n", (unsigned)-ret);
            goto cleanup;
        }
    }
    else
    {
        printf("[TLS] WARNING: no CA cert, skipping server verification (test only)\r\n");
    }

    /* 配置 SSL */
    int ret = mbedtls_ssl_config_defaults(&session->conf,
                                           MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
    {
        printf("[TLS] config_defaults fail: -0x%04x\r\n", (unsigned)-ret);
        goto cleanup;
    }

    /* 设置 CA 证书链 */
    if (ca_cert_der && ca_cert_len > 0)
    {
        mbedtls_ssl_conf_ca_chain(&session->conf, &session->cacert, NULL);
        mbedtls_ssl_conf_authmode(&session->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    }
    else
    {
        mbedtls_ssl_conf_authmode(&session->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    /* 设置 RNG */
    mbedtls_ssl_conf_rng(&session->conf, mbedtls_ctr_drbg_random, &s_ctr_drbg);

    /* SNI */
    ret = mbedtls_ssl_set_hostname(&session->ssl, hostname);
    if (ret != 0)
    {
        printf("[TLS] set_hostname fail: -0x%04x\r\n", (unsigned)-ret);
        goto cleanup;
    }

    /* 应用配置 */
    ret = mbedtls_ssl_setup(&session->ssl, &session->conf);
    if (ret != 0)
    {
        printf("[TLS] ssl_setup fail: -0x%04x\r\n", (unsigned)-ret);
        goto cleanup;
    }

    /* 设置 bio 回调 */
    mbedtls_ssl_set_bio(&session->ssl, session, bio_send, bio_recv, NULL);

    /* 设置 netconn recv 超时 */
    netconn_set_recvtimeout(conn, (int)timeout_ms);

    /* 执行握手 */
    printf("[TLS] handshake with %s ...\r\n", hostname);
    while ((ret = mbedtls_ssl_handshake(&session->ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            printf("[TLS] handshake fail: -0x%04x\r\n", (unsigned)-ret);
            goto cleanup;
        }
        /* WANT_READ/WANT_WRITE 是正常的，继续重试 */
    }

    session->active = true;

    /* 打印协商结果 */
    uint32_t flags = mbedtls_ssl_get_verify_result(&session->ssl);
    if (flags != 0 && (ca_cert_der && ca_cert_len > 0))
    {
        char vrfy_buf[512];
        mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "  ! ", flags);
        printf("[TLS] verify warnings:\r\n%s\r\n", vrfy_buf);
    }

    printf("[TLS] handshake OK: ciphersuite=%s\r\n",
           mbedtls_ssl_get_ciphersuite(&session->ssl));
    return HTTPS_TLS_OK;

cleanup:
    mbedtls_ssl_free(&session->ssl);
    mbedtls_ssl_config_free(&session->conf);
    mbedtls_x509_crt_free(&session->cacert);
    return HTTPS_TLS_ERR_HANDSHAKE;
}

/* ===== 加密发送 ===== */
int HttpsTls_Send(https_tls_session_t *session, const uint8_t *data, size_t len)
{
    if (!session || !session->active) return HTTPS_TLS_ERR_INIT;

    int ret;
    while ((ret = mbedtls_ssl_write(&session->ssl, data, len)) <= 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            printf("[TLS] send fail: -0x%04x\r\n", (unsigned)-ret);
            return HTTPS_TLS_ERR_SEND;
        }
    }
    return ret;
}

/* ===== 加密接收 ===== */
int HttpsTls_Recv(https_tls_session_t *session, uint8_t *buf, size_t max_len)
{
    if (!session || !session->active) return HTTPS_TLS_ERR_INIT;

    int ret;
    do {
        ret = mbedtls_ssl_read(&session->ssl, buf, max_len);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            continue;  /* 重试 */
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
            return 0;  /* EOF */
        }
        if (ret < 0)
        {
            printf("[TLS] recv fail: -0x%04x\r\n", (unsigned)-ret);
            return HTTPS_TLS_ERR_RECV;
        }
        break;
    } while (1);

    return ret;
}

/* ===== 关闭 ===== */
void HttpsTls_Close(https_tls_session_t *session)
{
    if (!session) return;

    if (session->active)
    {
        /* 发送 close_notify */
        mbedtls_ssl_close_notify(&session->ssl);
        session->active = false;
    }

    mbedtls_ssl_free(&session->ssl);
    mbedtls_ssl_config_free(&session->conf);
    mbedtls_x509_crt_free(&session->cacert);

    /* 关闭 TCP 连接 */
    if (session->conn)
    {
        netconn_delete(session->conn);
        session->conn = NULL;
    }
}
