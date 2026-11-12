/*
 * network_upgrade_downloader.c —— HTTP/HTTPS 分块下载器实现（LwIP netconn + mbedTLS）
 *
 * 实现流程：URL解析 → DNS → TCP连接 → [TLS握手] → HTTP GET → 收响应头 → 循环收body → 回调
 * 每块 ≤1016B（UPGRADE_STAGE_MAX_CHUNK_SIZE），回调里调 UpgradeEntry_Write。
 *
 * http:// → 走 netconn 明文路径（原有逻辑）
 * https:// → 走 mbedTLS 加密路径（新增）
 */
#include "network_upgrade_downloader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lwip/api.h"        /* netconn_* */
#include "lwip/dns.h"        /* netconn_gethostbyname */
#include "lwip/ip_addr.h"

/* HTTPS 支持 */
#include "https_tls.h"
#include "https_ca_cert.h"   /* 百度云 BOS 根 CA (GlobalSign Root CA - R1) */

/* ===== 内部缓冲（CCMRAM，不挤占普通 SRAM） ===== */
#define NW_DL_RX_BUF_SIZE  1024   /* 略大于 1016，收响应头和 body 复用 */
#define NW_DL_HTTP_PORT    80
#define NW_DL_HTTPS_PORT   443

/* ===== 全局连接引用（HTTP 路径用；HTTPS 路径由 TLS session 管理） ===== */
static struct netconn *g_download_conn;
static https_tls_session_t g_tls_session;
static bool g_using_tls;

/* ===== 关闭辅助 =====
 * HTTP 路径：netconn_delete
 * HTTPS 路径：HttpsTls_Close（内部已 netconn_delete）
 * 仅由升级任务调用；删除失败保留引用，下次下载先重试释放，不覆盖原始错误。
 */
static nw_dl_status_t download_close(nw_dl_status_t result)
{
    if (g_using_tls)
    {
        HttpsTls_Close(&g_tls_session);
        g_using_tls = false;
        return result;
    }

    err_t err = netconn_delete(g_download_conn);
    if (err == ERR_OK)
        g_download_conn = NULL;
    else
    {
        printf("[NWDL] connection delete fail err=%d\r\n", (int)err);
        if (result == NW_DL_OK)
            result = NW_DL_ERR_INTERNAL;
    }
    return result;
}

/* ===== URL 解析 =====
 * 支持 http:// 和 https://
 * 返回 0=成功，-1=格式错误
 * url 例：http://abc.baidubce.com:8080/fw/firmware.gwp
 *       https://abc.bcebos.com/firmware.gwp
 */
static int parse_url(const char *url, char *host, int host_max,
                     uint16_t *port, const char **path, bool *is_https)
{
    *is_https = false;

    if (strncmp(url, "https://", 8) == 0)
    {
        *is_https = true;
        *port = NW_DL_HTTPS_PORT;
    }
    else if (strncmp(url, "http://", 7) == 0)
    {
        *is_https = false;
        *port = NW_DL_HTTP_PORT;
    }
    else
    {
        return -1;
    }

    const char *p = url + (*is_https ? 8 : 7);
    const char *host_start = p;
    /* 找 host 结尾（: / 或 \0） */
    while (*p != ':' && *p != '/' && *p != '\0') p++;
    int host_len = p - host_start;
    if (host_len <= 0 || host_len >= host_max) return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    /* 解析端口（如有） */
    if (*p == ':')
    {
        *port = (uint16_t)atoi(p + 1);
        while (*p != '/' && *p != '\0') p++;
    }

    /* path */
    *path = (*p == '/') ? p : "/";
    return 0;
}

/* ===== HTTP 响应头解析（共享） ===== */

/* 从响应头缓冲找 HTTP 状态码 */
static int parse_http_status(const char *hdr, int hdr_len)
{
    /* "HTTP/1.1 200 OK" 或 "HTTP/1.1 206 Partial Content" */
    if (hdr_len < 12) return -1;
    if (strncmp(hdr, "HTTP/", 5) != 0) return -1;
    const char *p = hdr + 5;
    while (*p != ' ' && (p - hdr) < hdr_len) p++;
    while (*p == ' ' && (p - hdr) < hdr_len) p++;
    return atoi(p);
}

/* 从响应头找 Content-Length（大小写不敏感） */
static int32_t parse_content_length(const char *hdr, int hdr_len)
{
    const char *needle = "Content-Length:";
    const char *p = NULL;
    int needle_len = (int)strlen(needle);
    for (int i = 0; i <= hdr_len - needle_len; i++)
    {
        int match = 1;
        for (int j = 0; j < needle_len; j++)
        {
            char a = hdr[i + j];
            char b = needle[j];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (a != b) { match = 0; break; }
        }
        if (match) { p = hdr + i + needle_len; break; }
    }
    if (!p) return -1;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

/* ===== HTTPS 下载路径（mbedTLS） =====
 * TLS 给出平字节流，头和 body 都直接 recv 到缓冲。
 */
static nw_dl_status_t fetch_https(const nw_dl_params_t *params,
                                  const char *host, uint16_t port,
                                  const char *path, uint32_t chunk_size,
                                  uint32_t timeout_ms)
{
    ip_addr_t ip;
    err_t err;

    /* DNS */
    err = netconn_gethostbyname(host, &ip);
    if (err != ERR_OK)
    {
        printf("[NWDL] DNS fail: %s err=%d\r\n", host, (int)err);
        return NW_DL_ERR_DNS;
    }
    printf("[NWDL] DNS OK: %s\r\n", ipaddr_ntoa(&ip));

    /* TCP 连接 */
    struct netconn *conn = netconn_new(NETCONN_TCP);
    if (!conn)
    {
        printf("[NWDL] netconn_new fail\r\n");
        return NW_DL_ERR_CONNECT;
    }
    g_download_conn = conn;   /* TLS 接管后由 session 管理 */
    netconn_set_recvtimeout(conn, 10000);

    err = netconn_connect(conn, &ip, port);
    if (err != ERR_OK)
    {
        printf("[NWDL] connect fail err=%d\r\n", (int)err);
        nw_dl_status_t r = NW_DL_ERR_CONNECT;
        netconn_delete(conn);
        g_download_conn = NULL;
        return r;
    }
    printf("[NWDL] TCP connected (HTTPS)\r\n");

    /* TLS 握手，CA 证书 = 预置 GlobalSign Root CA (百度云 BOS) */
    https_tls_result_t tls_ret = HttpsTls_Connect(&g_tls_session, conn,
                                                    host,
                                                    g_https_ca_cert_der,
                                                    g_https_ca_cert_der_len,
                                                    timeout_ms);
    if (tls_ret != HTTPS_TLS_OK)
    {
        printf("[NWDL] TLS handshake fail: %d\r\n", (int)tls_ret);
        HttpsTls_Close(&g_tls_session);
        g_download_conn = NULL;
        return NW_DL_ERR_CONNECT;
    }
    g_using_tls = true;
    printf("[NWDL] TLS connected\r\n");

    /* 发 HTTP GET 请求（经 TLS 加密） */
    char req[256];
    int req_len;
    if (params->auth_token && params->auth_token[0])
    {
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\nHost: %s\r\nAuthorization: %s\r\nConnection: close\r\n\r\n",
            path, host, params->auth_token);
    }
    else
    {
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
            path, host);
    }
    if (req_len <= 0 || (size_t)req_len >= sizeof(req))
        return download_close(NW_DL_ERR_REQUEST);

    int sent = HttpsTls_Send(&g_tls_session, (const uint8_t *)req, (size_t)req_len);
    if (sent <= 0)
    {
        printf("[NWDL] TLS send request fail\r\n");
        return download_close(NW_DL_ERR_REQUEST);
    }

    /* 收响应头（TLS recv → hdr_buf，找 \r\n\r\n） */
    static char hdr_buf[512] __attribute__((section(".bss.ccmram")));
    int hdr_len = 0;
    int header_end_pos = -1;

    while (header_end_pos < 0 && hdr_len < (int)sizeof(hdr_buf) - 1)
    {
        int got = HttpsTls_Recv(&g_tls_session, (uint8_t *)hdr_buf + hdr_len,
                                 (size_t)(sizeof(hdr_buf) - 1 - hdr_len));
        if (got < 0)
        {
            printf("[NWDL] TLS recv header fail\r\n");
            return download_close(NW_DL_ERR_RESPONSE);
        }
        if (got == 0)
        {
            printf("[NWDL] TLS EOF during header\r\n");
            return download_close(NW_DL_ERR_RESPONSE);
        }
        hdr_len += got;

        /* 找 \r\n\r\n */
        for (int i = 3; i < hdr_len; i++)
        {
            if (hdr_buf[i-3] == '\r' && hdr_buf[i-2] == '\n' &&
                hdr_buf[i-1] == '\r' && hdr_buf[i] == '\n')
            {
                header_end_pos = i + 1;
                break;
            }
        }
    }
    hdr_buf[hdr_len] = '\0';

    if (header_end_pos < 0)
    {
        printf("[NWDL] header incomplete\r\n");
        return download_close(NW_DL_ERR_RESPONSE);
    }

    /* 解析状态码和 Content-Length */
    int status = parse_http_status(hdr_buf, hdr_len);
    if (status != 200 && status != 206)
    {
        printf("[NWDL] HTTP status %d\r\n", status);
        return download_close(NW_DL_ERR_HTTP_STATUS);
    }
    int32_t content_len = parse_content_length(hdr_buf, hdr_len);
    if (content_len < 0)
    {
        printf("[NWDL] no Content-Length\r\n");
        return download_close(NW_DL_ERR_NO_CONTENT_LEN);
    }
    if ((uint32_t)content_len != params->total_size)
    {
        printf("[NWDL] size mismatch: got %ld expected %u\r\n",
               (long)content_len, (unsigned)params->total_size);
        return download_close(NW_DL_ERR_SIZE_MISMATCH);
    }
    printf("[NWDL] HTTPS %d, Content-Length=%ld\r\n", status, (long)content_len);

    /* 收 body：先处理 header 同包残余（header_end_pos..hdr_len 之间的 body 数据） */
    static uint8_t chunk_buf[NW_DL_RX_BUF_SIZE] __attribute__((section(".bss.ccmram")));
    uint32_t offset = 0;
    uint32_t chunk_accum = 0;

    int body_in_hdr = hdr_len - header_end_pos;
    if (body_in_hdr > 0)
    {
        uint32_t copy = (uint32_t)body_in_hdr;
        if (copy > chunk_size) copy = chunk_size;
        memcpy(chunk_buf, hdr_buf + header_end_pos, copy);
        chunk_accum = copy;
        if (chunk_accum >= chunk_size || offset + chunk_accum >= (uint32_t)content_len)
        {
            nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum);
            if (cb_ret != NW_DL_OK)
            {
                printf("[NWDL] callback error at residual offset=%u\r\n", (unsigned)offset);
                return download_close(NW_DL_ERR_CALLBACK);
            }
            offset += chunk_accum;
            chunk_accum = 0;
        }
    }

    /* 循环收 body */
    while (offset < (uint32_t)content_len)
    {
        uint32_t space = chunk_size - chunk_accum;
        int got = HttpsTls_Recv(&g_tls_session, chunk_buf + chunk_accum, space);
        if (got < 0)
        {
            printf("[NWDL] TLS recv body fail at offset=%u\r\n", (unsigned)offset);
            return download_close(NW_DL_ERR_RECEIVE);
        }
        if (got == 0) break;  /* EOF */

        chunk_accum += (uint32_t)got;
        if (chunk_accum >= chunk_size || offset + chunk_accum >= (uint32_t)content_len)
        {
            nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum);
            if (cb_ret != NW_DL_OK)
            {
                printf("[NWDL] callback error at offset=%u\r\n", (unsigned)offset);
                return download_close(NW_DL_ERR_CALLBACK);
            }
            offset += chunk_accum;
            chunk_accum = 0;
        }
    }

    /* 残余 */
    if (chunk_accum > 0)
    {
        nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum);
        if (cb_ret != NW_DL_OK)
            return download_close(NW_DL_ERR_CALLBACK);
        offset += chunk_accum;
    }

    if (offset != (uint32_t)content_len)
    {
        printf("[NWDL] incomplete: got %u expected %ld\r\n", (unsigned)offset, (long)content_len);
        return download_close(NW_DL_ERR_RECEIVE);
    }

    nw_dl_status_t result = download_close(NW_DL_OK);
    if (result != NW_DL_OK) return result;
    printf("[NWDL] download complete: %u bytes\r\n", (unsigned)offset);
    return NW_DL_OK;
}

/* ===== HTTP 下载路径（原有逻辑，netconn 明文） ===== */
static nw_dl_status_t fetch_http(const nw_dl_params_t *params,
                                 const char *host, uint16_t port,
                                 const char *path, uint32_t chunk_size,
                                 uint32_t timeout_ms)
{
    ip_addr_t ip;
    err_t err;

    /* DNS */
    err = netconn_gethostbyname(host, &ip);
    if (err != ERR_OK)
    {
        printf("[NWDL] DNS fail: %s err=%d\r\n", host, (int)err);
        return NW_DL_ERR_DNS;
    }
    printf("[NWDL] DNS OK: %s\r\n", ipaddr_ntoa(&ip));

    /* TCP 连接 */
    struct netconn *conn = netconn_new(NETCONN_TCP);
    if (!conn)
    {
        printf("[NWDL] netconn_new fail\r\n");
        return NW_DL_ERR_CONNECT;
    }
    g_download_conn = conn;
    netconn_set_recvtimeout(conn, 10000);

    err = netconn_connect(conn, &ip, port);
    if (err != ERR_OK)
    {
        printf("[NWDL] connect fail err=%d\r\n", (int)err);
        return download_close(NW_DL_ERR_CONNECT);
    }
    printf("[NWDL] TCP connected\r\n");

    /* 发 HTTP GET */
    char req[256];
    int req_len;
    if (params->auth_token && params->auth_token[0])
    {
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\nHost: %s\r\nAuthorization: %s\r\nConnection: close\r\n\r\n",
            path, host, params->auth_token);
    }
    else
    {
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
            path, host);
    }
    if (req_len <= 0 || (size_t)req_len >= sizeof(req))
        return download_close(NW_DL_ERR_REQUEST);
    err = netconn_write(conn, req, req_len, NETCONN_COPY);
    if (err != ERR_OK)
    {
        printf("[NWDL] write request fail err=%d\r\n", (int)err);
        return download_close(NW_DL_ERR_REQUEST);
    }

    /* 收响应头 */
    static char hdr_buf[512] __attribute__((section(".bss.ccmram")));
    int hdr_len = 0;
    int header_end_pos = -1;
    struct netbuf *nbuf = NULL;

    static uint8_t body_residual[NW_DL_RX_BUF_SIZE] __attribute__((section(".bss.ccmram")));
    uint32_t body_residual_len = 0;

    while (header_end_pos < 0 && hdr_len < (int)sizeof(hdr_buf) - 1)
    {
        err = netconn_recv(conn, &nbuf);
        if (err != ERR_OK)
        {
            printf("[NWDL] recv header fail err=%d\r\n", (int)err);
            return download_close(NW_DL_ERR_RESPONSE);
        }
        void *data;
        uint16_t dlen;
        netbuf_data(nbuf, &data, &dlen);
        int copy = dlen;
        if (hdr_len + copy >= (int)sizeof(hdr_buf)) copy = sizeof(hdr_buf) - 1 - hdr_len;
        memcpy(hdr_buf + hdr_len, data, copy);
        hdr_len += copy;

        for (int i = 3; i < hdr_len; i++)
        {
            if (hdr_buf[i-3] == '\r' && hdr_buf[i-2] == '\n' &&
                hdr_buf[i-1] == '\r' && hdr_buf[i] == '\n')
            {
                header_end_pos = i + 1;
                break;
            }
        }

        if (header_end_pos >= 0)
        {
            int residual_start = header_end_pos;
            if (copy < dlen)
            {
                uint16_t remain = dlen - copy;
                if (remain > sizeof(body_residual)) remain = sizeof(body_residual);
                memcpy(body_residual, (uint8_t *)data + copy, remain);
                body_residual_len = remain;
            }
            else if (residual_start < hdr_len)
            {
                int remain = hdr_len - residual_start;
                if (remain > (int)sizeof(body_residual)) remain = sizeof(body_residual);
                memcpy(body_residual, hdr_buf + residual_start, remain);
                body_residual_len = (uint32_t)remain;
                hdr_len = residual_start;
            }
        }

        netbuf_delete(nbuf);
        nbuf = NULL;
    }
    hdr_buf[hdr_len] = '\0';

    if (header_end_pos < 0)
    {
        printf("[NWDL] header incomplete\r\n");
        return download_close(NW_DL_ERR_RESPONSE);
    }

    int status = parse_http_status(hdr_buf, hdr_len);
    if (status != 200 && status != 206)
    {
        printf("[NWDL] HTTP status %d\r\n", status);
        return download_close(NW_DL_ERR_HTTP_STATUS);
    }
    int32_t content_len = parse_content_length(hdr_buf, hdr_len);
    if (content_len < 0)
    {
        printf("[NWDL] no Content-Length\r\n");
        return download_close(NW_DL_ERR_NO_CONTENT_LEN);
    }
    if ((uint32_t)content_len != params->total_size)
    {
        printf("[NWDL] size mismatch: got %ld expected %u\r\n",
               (long)content_len, (unsigned)params->total_size);
        return download_close(NW_DL_ERR_SIZE_MISMATCH);
    }
    printf("[NWDL] HTTP %d, Content-Length=%ld\r\n", status, (long)content_len);

    /* 收 body */
    static uint8_t chunk_buf[NW_DL_RX_BUF_SIZE] __attribute__((section(".bss.ccmram")));
    uint32_t offset = 0;
    uint32_t chunk_accum = 0;

    if (body_residual_len > 0)
    {
        uint32_t copy = body_residual_len;
        if (copy > chunk_size) copy = chunk_size;
        memcpy(chunk_buf, body_residual, copy);
        chunk_accum = copy;
        if (chunk_accum >= chunk_size || offset + chunk_accum >= (uint32_t)content_len)
        {
            nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum);
            if (cb_ret != NW_DL_OK)
            {
                printf("[NWDL] callback error at residual offset=%u\r\n", (unsigned)offset);
                return download_close(NW_DL_ERR_CALLBACK);
            }
            offset += chunk_accum;
            chunk_accum = 0;
        }
    }

    while (offset < (uint32_t)content_len)
    {
        err = netconn_recv(conn, &nbuf);
        if (err != ERR_OK)
        {
            if (err == ERR_CLSD || err == ERR_CONN) break;
            printf("[NWDL] recv body fail err=%d at offset=%u\r\n", (int)err, (unsigned)offset);
            return download_close(NW_DL_ERR_RECEIVE);
        }

        void *seg_data = NULL;
        uint16_t seg_len = 0;

        do {
            netbuf_data(nbuf, &seg_data, &seg_len);
            uint16_t seg_consumed = 0;
            while (seg_consumed < seg_len && offset + chunk_accum < (uint32_t)content_len)
            {
                uint16_t avail = seg_len - seg_consumed;
                uint32_t space = chunk_size - chunk_accum;
                uint32_t copy = (avail < space) ? avail : space;

                memcpy(chunk_buf + chunk_accum, (uint8_t *)seg_data + seg_consumed, copy);
                chunk_accum += copy;
                seg_consumed += (uint16_t)copy;

                if (chunk_accum >= chunk_size || offset + chunk_accum >= (uint32_t)content_len)
                {
                    nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum);
                    if (cb_ret != NW_DL_OK)
                    {
                        printf("[NWDL] callback error at offset=%u\r\n", (unsigned)offset);
                        netbuf_delete(nbuf);
                        return download_close(NW_DL_ERR_CALLBACK);
                    }
                    offset += chunk_accum;
                    chunk_accum = 0;
                }
            }
        } while (netbuf_next(nbuf) == ERR_OK);

        netbuf_delete(nbuf);
        nbuf = NULL;
    }

    if (chunk_accum > 0)
    {
        nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum);
        if (cb_ret != NW_DL_OK)
            return download_close(NW_DL_ERR_CALLBACK);
        offset += chunk_accum;
    }

    if (offset != (uint32_t)content_len)
    {
        printf("[NWDL] incomplete: got %u expected %ld\r\n", (unsigned)offset, (long)content_len);
        return download_close(NW_DL_ERR_RECEIVE);
    }

    nw_dl_status_t result = download_close(NW_DL_OK);
    if (result != NW_DL_OK) return result;
    printf("[NWDL] download complete: %u bytes\r\n", (unsigned)offset);
    return NW_DL_OK;
}

/* ===== 入口：解析 URL，分发 HTTP / HTTPS ===== */
nw_dl_status_t NetworkDownloader_Fetch(const nw_dl_params_t *params)
{
    char host[64];
    uint16_t port;
    const char *path;
    bool is_https;

    if (!params || !params->url || !params->cb) return NW_DL_ERR_URL;

    /* 清理上次残留连接 */
    if (download_close(NW_DL_OK) != NW_DL_OK) return NW_DL_ERR_INTERNAL;

    uint32_t chunk_size = params->chunk_size;
    if (chunk_size == 0 || chunk_size > 1016) chunk_size = 1016;
    uint32_t timeout_ms = params->timeout_ms;
    if (timeout_ms == 0) timeout_ms = 120000;

    /* 解析 URL */
    if (parse_url(params->url, host, sizeof(host), &port, &path, &is_https) != 0)
    {
        printf("[NWDL] URL parse fail: %s\r\n", params->url);
        return NW_DL_ERR_URL;
    }
    printf("[NWDL] %s host=%s port=%u path=%s\r\n",
           is_https ? "HTTPS" : "HTTP", host, (unsigned)port, path);

    if (is_https)
    {
        return fetch_https(params, host, port, path, chunk_size, timeout_ms);
    }
    else
    {
        return fetch_http(params, host, port, path, chunk_size, timeout_ms);
    }
}

const char *NetworkDownloader_ErrorString(nw_dl_status_t status)
{
    switch (status)
    {
        case NW_DL_OK:                 return "ok";
        case NW_DL_ERR_URL:            return "E_URL";
        case NW_DL_ERR_DNS:            return "E_DNS";
        case NW_DL_ERR_CONNECT:        return "E_CONNECT";
        case NW_DL_ERR_REQUEST:        return "E_REQUEST";
        case NW_DL_ERR_RESPONSE:       return "E_RESPONSE";
        case NW_DL_ERR_HTTP_STATUS:    return "E_HTTP_STATUS";
        case NW_DL_ERR_NO_CONTENT_LEN: return "E_NO_CONTENT_LEN";
        case NW_DL_ERR_SIZE_MISMATCH:  return "E_SIZE_MISMATCH";
        case NW_DL_ERR_RECEIVE:        return "E_RECEIVE";
        case NW_DL_ERR_CALLBACK:       return "E_CALLBACK";
        case NW_DL_ERR_TIMEOUT:        return "E_TIMEOUT";
        case NW_DL_ERR_INTERNAL:       return "E_INTERNAL";
        default:                       return "E_UNKNOWN";
    }
}
