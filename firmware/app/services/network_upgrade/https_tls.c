/*
 * https_tls.c —— HTTPS TLS 客户端包装器实现
 *
 * 在已有 TCP netconn 上叠加 TLS1.2（ECDHE-RSA-AES128-GCM-SHA256）。
 * mbedTLS 通过 bio 回调收发加密记录，bio 内部走 netconn_write/recv。
 *
 * 架构层次：
 *   ┌─────────────────────────────────────┐
 *   │ network_upgrade_downloader.c        │  应用层：HTTP GET / 收 body
 *   ├─────────────────────────────────────┤
 *   │ https_tls.c (本文件)                │  包装层：握手/加密发送/加密接收
 *   ├─────────────────────────────────────┤
 *   │ mbedTLS (ssl_tls/ssl_msg/...)       │  TLS 协议层：记录加密/解密/握手
 *   ├─────────────────────────────────────┤
 *   │ bio_send / bio_recv (回调)          │  BIO 层：mbedTLS ←→ netconn 桥接
 *   ├─────────────────────────────────────┤
 *   │ lwIP netconn (TCP)                  │  传输层：TCP 可靠传输
 *   └─────────────────────────────────────┘
 *
 * 模块生命周期：
 *   HttpsTls_Init()     一次性初始化（熵源 + CTR_DRBG + 时间注册）
 *   HttpsTls_Connect()  每次下载新建 TLS 会话（握手 + 证书校验）
 *   HttpsTls_Send/Recv  加密收发数据
 *   HttpsTls_Close()    关闭会话（close_notify + 释放资源）
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

/* ===== TLS 握手 =====
 * 在已建立的 TCP 连接上执行 TLS 握手，完成后可加密收发。
 *
 * 握手流程（mbedTLS 内部自动处理，这里只做配置和驱动）：
 *   ClientHello → ServerHello → Certificate → ServerKeyExchange →
 *   ServerHelloDone → ClientKeyExchange → ChangeCipherSpec → Finished
 *
 * 握手时 mbedTLS 通过 bio 回调收发 TLS 记录（bio_send/bio_recv）。
 * 证书校验：用预置的 CA 证书验证服务器证书链。
 *
 * 参数：
 *   conn         已连接的 TCP netconn
 *   hostname     服务器域名（用于 SNI + 证书 CN 校验）
 *   ca_cert_der  CA 根证书 DER 数据（NULL=跳过校验，仅测试用）
 *   ca_cert_len  CA 证书长度
 *   timeout_ms   netconn 收超时（握手期间）
 */
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

    /* 重置 bio 残余缓冲（上次会话可能残留数据） */
    s_bio_buf.len = 0;
    s_bio_buf.offset = 0;

    /* 1. 初始化 SSL 上下文（ssl + config + CA 证书三个结构） */
    mbedtls_ssl_init(&session->ssl);
    mbedtls_ssl_config_init(&session->conf);
    mbedtls_x509_crt_init(&session->cacert);

    /* 2. 加载 CA 证书（DER → mbedTLS 内部结构）
     * nocopy 模式：直接引用 Flash 中的 DER 数据，不额外分配 RAM */
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
        /* 无 CA 证书 = 跳过证书校验（仅测试用，生产环境必须传 CA） */
        printf("[TLS] WARNING: no CA cert, skipping server verification (test only)\r\n");
    }

    /* 3. 配置 SSL：客户端模式 + TLS1.2 流式传输 + 默认安全预设 */
    int ret = mbedtls_ssl_config_defaults(&session->conf,
                                           MBEDTLS_SSL_IS_CLIENT,        /* 客户端 */
                                           MBEDTLS_SSL_TRANSPORT_STREAM, /* TLS（非 DTLS） */
                                           MBEDTLS_SSL_PRESET_DEFAULT);  /* 默认安全等级 */
    if (ret != 0)
    {
        printf("[TLS] config_defaults fail: -0x%04x\r\n", (unsigned)-ret);
        goto cleanup;
    }

    /* 4. 设置证书校验策略 */
    if (ca_cert_der && ca_cert_len > 0)
    {
        /* 有 CA 证书：设置 CA 链 + 强制校验（VERIFY_REQUIRED）
         * 服务器证书必须由预置 CA 签发，否则握手失败 */
        mbedtls_ssl_conf_ca_chain(&session->conf, &session->cacert, NULL);
        mbedtls_ssl_conf_authmode(&session->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    }
    else
    {
        /* 无 CA 证书：不校验（VERIFY_NONE），任何人可中间人，仅测试用 */
        mbedtls_ssl_conf_authmode(&session->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    /* 5. 设置 RNG（随机数生成器）：CTR_DRBG（基于熵源的确定性随机）
     * TLS 握手需要随机数生成 ClientHello.random 和密钥材料 */
    mbedtls_ssl_conf_rng(&session->conf, mbedtls_ctr_drbg_random, &s_ctr_drbg);

    /* 6. SNI（Server Name Indication）：把域名发给服务器
     * 服务器根据 SNI 选择对应证书（一个 IP 托管多域名时需要） */
    ret = mbedtls_ssl_set_hostname(&session->ssl, hostname);
    if (ret != 0)
    {
        printf("[TLS] set_hostname fail: -0x%04x\r\n", (unsigned)-ret);
        goto cleanup;
    }

    /* 7. 应用配置到 SSL 上下文（分配发送/接收缓冲等资源） */
    ret = mbedtls_ssl_setup(&session->ssl, &session->conf);
    if (ret != 0)
    {
        printf("[TLS] ssl_setup fail: -0x%04x\r\n", (unsigned)-ret);
        goto cleanup;
    }

    /* 8. 设置 BIO 回调：mbedTLS 通过 bio_send/bio_recv 收发 TLS 记录 */
    mbedtls_ssl_set_bio(&session->ssl, session, bio_send, bio_recv, NULL);

    /* 9. 设置 netconn 收超时（握手期间） */
    netconn_set_recvtimeout(conn, (int)timeout_ms);

    /* 10. 执行 TLS 握手（阻塞直到完成或失败）
     * WANT_READ/WANT_WRITE 是正常的非阻塞返回，继续重试即可 */
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

    /* 11. 检查证书校验结果（即使 VERIFY_REQUIRED，也可能有警告） */
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
    /* 握手失败：释放 SSL 资源（TCP 连接由调用方关闭） */
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
