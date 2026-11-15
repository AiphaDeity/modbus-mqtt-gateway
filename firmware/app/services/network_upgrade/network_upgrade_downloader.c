/*
 * network_upgrade_downloader.c —— HTTP/HTTPS 分块下载器实现（LwIP netconn + mbedTLS）
 *
 * 实现流程：URL解析 → DNS → TCP连接 → [TLS握手] → HTTP GET → 收响应头 → 循环收body → 回调
 * 每块 ≤1016B（UPGRADE_STAGE_MAX_CHUNK_SIZE），回调里调 UpgradeEntry_Write。
 *
 * http://使用netconn；https://在同一连接上完成TLS认证与加密。
 */
#include "network_upgrade_downloader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "app_monitor.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os2.h"

#include "lwip/api.h"        /* netconn_* */
#include "lwip/dns.h"        /* netconn_gethostbyname */
#include "lwip/ip_addr.h"

/* HTTPS 支持 */
#include "https_tls.h"
#include "https_ca_cert.h"   /* 百度BOS根证书DigiCert Global Root G2 */

/* ===== 内部缓冲（CCMRAM，不挤占普通 SRAM） ===== */
#define NW_DL_RX_BUF_SIZE  1024   /* 略大于 1016，收响应头和 body 复用 */
#define NW_DL_HTTP_PORT    80
#define NW_DL_HTTPS_PORT   443
#define NW_DL_DNS_TIMEOUT_MS 20000U
#define NW_DL_IO_TIMEOUT_MS 10000U

/* ===== 全局连接引用（HTTP 路径用；HTTPS 路径由 TLS session 管理） ===== */
static struct netconn *g_download_conn;
static https_tls_session_t g_tls_session;
static bool g_using_tls;
static uint32_t g_started_ms;
static uint32_t g_timeout_ms;
static volatile bool g_connect_completed;
// HTTP与HTTPS由同一升级任务串行执行，共用单行响应头和分块缓冲。
static char g_header_buf[512] __attribute__((section(".bss.ccmram")));
static uint8_t g_chunk_buf[NW_DL_RX_BUF_SIZE] __attribute__((section(".bss.ccmram")));

// DNS、连接、收发与回调共同使用本次下载的固定期限。
static uint32_t download_remaining(void)
{
    uint32_t elapsed = HAL_GetTick() - g_started_ms;
    return elapsed < g_timeout_ms ? g_timeout_ms - elapsed : 0U;
}

// 阻塞前只声明本次剩余期限，不通过等待重置总超时。
static bool download_wait(uint32_t limit_ms)
{
    uint32_t remaining = download_remaining();
    uint32_t wait_ms = remaining < limit_ms ? remaining : limit_ms;
    return wait_ms != 0U && Monitor_UpgradeWait(wait_ms);
}

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
        if (g_tls_session.conn != NULL)
            return result == NW_DL_OK ? NW_DL_ERR_INTERNAL : result;
        g_download_conn = NULL;
        g_using_tls = false;
        return result;
    }

    if (g_download_conn == NULL) return result;
    uint32_t remaining = download_remaining();
    uint32_t wait_ms = remaining == 0U ? 1U : remaining < NW_DL_IO_TIMEOUT_MS ? remaining : NW_DL_IO_TIMEOUT_MS;
    netconn_set_sendtimeout(g_download_conn, wait_ms);
    if (!Monitor_UpgradeWait(wait_ms)) return result == NW_DL_OK ? NW_DL_ERR_INTERNAL : result;
    err_t err = netconn_delete(g_download_conn);
    (void)Monitor_UpgradeProgress();
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

// 连接完成或出错只记录事件，TCP/IP线程内不执行升级或阻塞操作。
static void download_event(struct netconn *conn, enum netconn_evt event, u16_t len)
{
    (void)len;
    if (conn == g_download_conn &&
        (event == NETCONN_EVT_SENDPLUS || event == NETCONN_EVT_ERROR))
        g_connect_completed = true;
}

// DNS按当前LwIP两服务器重试预算等待，TCP采用非阻塞连接避免SYN重传超出期限。
static nw_dl_status_t download_connect(const char *host, uint16_t port)
{
    ip_addr_t ip;
    if (download_remaining() < NW_DL_DNS_TIMEOUT_MS || !download_wait(NW_DL_DNS_TIMEOUT_MS))
        return NW_DL_ERR_TIMEOUT;
    err_t err = netconn_gethostbyname(host, &ip);
    if (!Monitor_UpgradeProgress() || download_remaining() == 0U) return NW_DL_ERR_TIMEOUT;
    if (err != ERR_OK)
    {
        printf("[NWDL] DNS fail host=%s err=%d\r\n", host, (int)err);
        return NW_DL_ERR_DNS;
    }
    printf("[NWDL] DNS OK: %s\r\n", ipaddr_ntoa(&ip));
    g_download_conn = netconn_new_with_callback(NETCONN_TCP, download_event);
    if (g_download_conn == NULL) return NW_DL_ERR_CONNECT;
    netconn_set_nonblocking(g_download_conn, 1);
    g_connect_completed = false;
    uint32_t wait_ms = download_remaining();
    if (wait_ms > NW_DL_IO_TIMEOUT_MS) wait_ms = NW_DL_IO_TIMEOUT_MS;
    if (!download_wait(wait_ms)) return download_close(NW_DL_ERR_TIMEOUT);
    uint32_t started_ms = HAL_GetTick();
    err = netconn_connect(g_download_conn, &ip, port);
    if (err == ERR_INPROGRESS)
    {
        while (!g_connect_completed && download_remaining() != 0U &&
               (uint32_t)(HAL_GetTick() - started_ms) < wait_ms)
        {
            uint32_t ticks = (osKernelGetTickFreq() + 99U) / 100U;
            if (osDelay(ticks) != osOK) break;
        }
        err = g_connect_completed ? netconn_err(g_download_conn) : ERR_TIMEOUT;
    }
    netconn_set_nonblocking(g_download_conn, 0);
    if (!Monitor_UpgradeProgress() || download_remaining() == 0U) err = ERR_TIMEOUT;
    if (err != ERR_OK)
    {
        printf("[NWDL] connect fail err=%d\r\n", (int)err);
        return download_close(err == ERR_TIMEOUT ? NW_DL_ERR_TIMEOUT : NW_DL_ERR_CONNECT);
    }
    printf("[NWDL] TCP connected\r\n");
    return NW_DL_OK;
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

typedef struct
{
    bool status_seen;
    bool length_seen;
    bool complete;
} nw_http_header_t;

// 逐行核验整包响应；拒绝重复长度和分块编码，不累积整份响应头。
static nw_dl_status_t parse_header_line(char *line, int len, uint32_t expected,
                                        nw_http_header_t *header)
{
    if (len < 2 || line[len - 2] != '\r' || line[len - 1] != '\n')
        return NW_DL_ERR_RESPONSE;
    len -= 2;
    if (!header->status_seen)
    {
        if (len < 13 || memcmp(line, "HTTP/1.", 7) != 0 ||
            (line[7] != '0' && line[7] != '1') || line[8] != ' ' || line[12] != ' ')
            return NW_DL_ERR_RESPONSE;
        if (memcmp(line + 9, "200", 3) != 0)
        {
            printf("[NWDL] HTTP status %.3s\r\n", line + 9);
            return NW_DL_ERR_HTTP_STATUS;
        }
        header->status_seen = true;
        return NW_DL_OK;
    }
    if (len == 0)
    {
        if (!header->length_seen) return NW_DL_ERR_NO_CONTENT_LEN;
        header->complete = true;
        return NW_DL_OK;
    }

    int colon = 0;
    while (colon < len && line[colon] != ':')
    {
        unsigned char c = (unsigned char)line[colon];
        if (c <= ' ' || c >= 127U) return NW_DL_ERR_RESPONSE;
        if (c >= 'A' && c <= 'Z') line[colon] = (char)(c + ('a' - 'A'));
        ++colon;
    }
    if (colon == 0 || colon == len) return NW_DL_ERR_RESPONSE;
    line[colon] = '\0';
    if (strcmp(line, "transfer-encoding") == 0) return NW_DL_ERR_RESPONSE;
    if (strcmp(line, "content-length") != 0) return NW_DL_OK;
    if (header->length_seen) return NW_DL_ERR_RESPONSE;

    int pos = colon + 1;
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    if (pos == len || line[pos] < '0' || line[pos] > '9') return NW_DL_ERR_RESPONSE;
    uint32_t length = 0U;
    while (pos < len && line[pos] >= '0' && line[pos] <= '9')
    {
        uint32_t digit = (uint32_t)(line[pos++] - '0');
        if (length > (UINT32_MAX - digit) / 10U) return NW_DL_ERR_RESPONSE;
        length = length * 10U + digit;
    }
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    if (pos != len) return NW_DL_ERR_RESPONSE;
    if (length != expected) return NW_DL_ERR_SIZE_MISMATCH;
    header->length_seen = true;
    return NW_DL_OK;
}

// 连续消费所有netbuf段；头结束后的字节直接进入原分块缓冲，不另存残余副本。
static nw_dl_status_t receive_response(struct netconn *conn, const nw_dl_params_t *params,
                                       uint32_t chunk_size)
{
    int hdr_len = 0;
    nw_http_header_t header = {0};
    uint32_t offset = 0;
    uint32_t chunk_accum = 0;
    nw_dl_status_t result = NW_DL_OK;

    while ((result == NW_DL_OK) && (!header.complete || (offset < params->total_size)))
    {
        struct netbuf *nbuf = NULL;
        uint32_t wait_ms = download_remaining();
        if (wait_ms > NW_DL_IO_TIMEOUT_MS) wait_ms = NW_DL_IO_TIMEOUT_MS;
        netconn_set_recvtimeout(conn, wait_ms);
        if (!download_wait(wait_ms)) return NW_DL_ERR_TIMEOUT;
        err_t err = netconn_recv(conn, &nbuf);
        if (!Monitor_UpgradeProgress() || download_remaining() == 0U)
        {
            if (nbuf != NULL) netbuf_delete(nbuf);
            return NW_DL_ERR_TIMEOUT;
        }
        if ((err != ERR_OK) || (nbuf == NULL))
        {
            printf("[NWDL] recv %s fail err=%d at=%u expected=%u\r\n",
                   header.complete ? "body" : "header", (int)err,
                   (unsigned)(offset + chunk_accum), (unsigned)params->total_size);
            if (nbuf != NULL) netbuf_delete(nbuf);
            result = header.complete ? NW_DL_ERR_RECEIVE : NW_DL_ERR_RESPONSE;
            break;
        }

        do
        {
            void *data = NULL;
            uint16_t len = 0;
            if ((netbuf_data(nbuf, &data, &len) != ERR_OK) || ((len != 0U) && (data == NULL)))
            {
                result = header.complete ? NW_DL_ERR_RECEIVE : NW_DL_ERR_RESPONSE;
                break;
            }
            uint16_t consumed = 0;
            while (!header.complete && (consumed < len))
            {
                if (hdr_len >= (int)sizeof(g_header_buf) - 1)
                {
                    result = NW_DL_ERR_RESPONSE;
                    break;
                }
                g_header_buf[hdr_len++] = ((const char *)data)[consumed++];
                if ((hdr_len >= 2) && (g_header_buf[hdr_len - 2] == '\r') &&
                    (g_header_buf[hdr_len - 1] == '\n'))
                {
                    result = parse_header_line(g_header_buf, hdr_len, params->total_size, &header);
                    hdr_len = 0;
                }
                if (result != NW_DL_OK) break;
            }
            if (result != NW_DL_OK) break;

            while (consumed < len)
            {
                uint32_t available = len - consumed;
                uint32_t remaining = params->total_size - offset - chunk_accum;
                if (available > remaining)
                {
                    result = NW_DL_ERR_SIZE_MISMATCH;
                    break;
                }
                uint32_t space = chunk_size - chunk_accum;
                uint32_t copy = available < space ? available : space;
                memcpy(g_chunk_buf + chunk_accum, (const uint8_t *)data + consumed, copy);
                consumed += (uint16_t)copy;
                chunk_accum += copy;

                if ((chunk_accum == chunk_size) || (offset + chunk_accum == params->total_size))
                {
                    if (params->cb(params->ctx, offset, g_chunk_buf, chunk_accum) != NW_DL_OK)
                    {
                        result = NW_DL_ERR_CALLBACK;
                        break;
                    }
                    offset += chunk_accum;
                    chunk_accum = 0;
                }
            }
            // netbuf_next返回1表示已移到最后一段；只有负数才表示没有下一段。
        } while ((result == NW_DL_OK) && (netbuf_next(nbuf) >= 0));

        netbuf_delete(nbuf);
    }

    if (result != NW_DL_OK)
    {
        printf("[NWDL] response fail: %s header_line=%d body=%u expected=%u\r\n",
               NetworkDownloader_ErrorString(result), hdr_len,
               (unsigned)(offset + chunk_accum), (unsigned)params->total_size);
    }
    return result;
}

/* ===== HTTPS 下载路径（mbedTLS） =====
 * TLS 给出平字节流，头和 body 都直接 recv 到缓冲。
 */
static nw_dl_status_t fetch_https(const nw_dl_params_t *params,
                                  const char *host, uint16_t port,
                                  const char *path, uint32_t chunk_size)
{
    nw_dl_status_t connected = download_connect(host, port);
    if (connected != NW_DL_OK) return connected;
    struct netconn *conn = g_download_conn;
    uint32_t remaining = download_remaining();
    if (remaining == 0U) return download_close(NW_DL_ERR_TIMEOUT);

    /* 按BOS服务器域名和预置CA执行证书校验及握手。 */
    https_tls_result_t tls_ret = HttpsTls_Connect(&g_tls_session, conn,
                                                    host,
                                                    g_https_ca_cert_der,
                                                    g_https_ca_cert_der_len,
                                                    remaining);
    if (tls_ret != HTTPS_TLS_OK)
    {
        printf("[NWDL] TLS handshake fail: %d\r\n", (int)tls_ret);
        g_using_tls = g_tls_session.conn != NULL;
        return download_close(tls_ret == HTTPS_TLS_ERR_TIMEOUT ? NW_DL_ERR_TIMEOUT : NW_DL_ERR_CONNECT);
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
    if (sent != req_len)
    {
        printf("[NWDL] TLS send request fail\r\n");
        return download_close(sent == -HTTPS_TLS_ERR_TIMEOUT ? NW_DL_ERR_TIMEOUT : NW_DL_ERR_REQUEST);
    }

    // 每收到完整一行就解析并释放空间，头结束后的残余字节仍按原顺序交给body。
    nw_http_header_t header = {0};
    int hdr_len = 0;
    int header_end_pos = 0;
    while (!header.complete)
    {
        if (hdr_len >= (int)sizeof(g_header_buf) - 1)
            return download_close(NW_DL_ERR_RESPONSE);
        int got = HttpsTls_Recv(&g_tls_session, (uint8_t *)g_header_buf + hdr_len,
                                 (size_t)(sizeof(g_header_buf) - 1 - hdr_len));
        if (got <= 0)
        {
            printf("[NWDL] TLS header incomplete: %d\r\n", got);
            return download_close(got == -HTTPS_TLS_ERR_TIMEOUT ? NW_DL_ERR_TIMEOUT : NW_DL_ERR_RESPONSE);
        }
        hdr_len += got;
        int line_start = 0;
        for (int i = 1; i < hdr_len; ++i)
        {
            if (g_header_buf[i - 1] != '\r' || g_header_buf[i] != '\n') continue;
            nw_dl_status_t result = parse_header_line(g_header_buf + line_start, i + 1 - line_start,
                                                       params->total_size, &header);
            if (result != NW_DL_OK)
            {
                printf("[NWDL] header fail: %s\r\n", NetworkDownloader_ErrorString(result));
                return download_close(result);
            }
            line_start = i + 1;
            if (header.complete)
            {
                header_end_pos = line_start;
                break;
            }
        }
        if (!header.complete && line_start != 0)
        {
            hdr_len -= line_start;
            memmove(g_header_buf, g_header_buf + line_start, (size_t)hdr_len);
        }
    }
    uint32_t content_len = params->total_size;

    /* 收 body：先处理 header 同包残余（header_end_pos..hdr_len 之间的 body 数据） */
    uint32_t offset = 0;
    uint32_t chunk_accum = 0;

    uint32_t body_in_hdr = (uint32_t)(hdr_len - header_end_pos);
    if (body_in_hdr > params->total_size)
        return download_close(NW_DL_ERR_SIZE_MISMATCH);
    uint32_t consumed = 0U;
    while (consumed < body_in_hdr)
    {
        uint32_t copy = body_in_hdr - consumed;
        uint32_t space = chunk_size - chunk_accum;
        if (copy > space) copy = space;
        memcpy(g_chunk_buf + chunk_accum, g_header_buf + header_end_pos + consumed, copy);
        chunk_accum += copy;
        consumed += copy;
        if (chunk_accum >= chunk_size || offset + chunk_accum >= (uint32_t)content_len)
        {
            nw_dl_status_t cb_ret = params->cb(params->ctx, offset, g_chunk_buf, chunk_accum);
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
        uint32_t remaining = (uint32_t)content_len - offset - chunk_accum;
        if (space > remaining) space = remaining;
        int got = HttpsTls_Recv(&g_tls_session, g_chunk_buf + chunk_accum, space);
        if (got < 0)
        {
            printf("[NWDL] TLS recv body fail at offset=%u\r\n", (unsigned)offset);
            return download_close(got == -HTTPS_TLS_ERR_TIMEOUT ? NW_DL_ERR_TIMEOUT : NW_DL_ERR_RECEIVE);
        }
        if (got == 0) break;  /* EOF */

        chunk_accum += (uint32_t)got;
        if (chunk_accum >= chunk_size || offset + chunk_accum >= (uint32_t)content_len)
        {
            nw_dl_status_t cb_ret = params->cb(params->ctx, offset, g_chunk_buf, chunk_accum);
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
        nw_dl_status_t cb_ret = params->cb(params->ctx, offset, g_chunk_buf, chunk_accum);
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

/* ===== HTTP 下载路径（netconn 明文） ===== */
static nw_dl_status_t fetch_http(const nw_dl_params_t *params,
                                 const char *host, uint16_t port,
                                 const char *path, uint32_t chunk_size)
{
    nw_dl_status_t connected = download_connect(host, port);
    if (connected != NW_DL_OK) return connected;
    struct netconn *conn = g_download_conn;

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
    uint32_t wait_ms = download_remaining();
    if (wait_ms > NW_DL_IO_TIMEOUT_MS) wait_ms = NW_DL_IO_TIMEOUT_MS;
    netconn_set_sendtimeout(conn, wait_ms);
    if (!download_wait(wait_ms)) return download_close(NW_DL_ERR_TIMEOUT);
    err_t err = netconn_write(conn, req, req_len, NETCONN_COPY);
    if (!Monitor_UpgradeProgress() || download_remaining() == 0U)
        return download_close(NW_DL_ERR_TIMEOUT);
    if (err != ERR_OK)
    {
        printf("[NWDL] write request fail err=%d\r\n", (int)err);
        return download_close(NW_DL_ERR_REQUEST);
    }

    nw_dl_status_t result = receive_response(conn, params, chunk_size);
    result = download_close(result);
    if (result == NW_DL_OK)
        printf("[NWDL] download complete: %u bytes\r\n", (unsigned)params->total_size);
    return result;
}

/* ===== 入口：解析 URL，分发 HTTP / HTTPS ===== */
nw_dl_status_t NetworkDownloader_Fetch(const nw_dl_params_t *params)
{
    char host[64];
    uint16_t port;
    const char *path;
    bool is_https;

    if (!params || !params->url || !params->cb) return NW_DL_ERR_URL;
    if (params->total_size == 0U) return NW_DL_ERR_SIZE_MISMATCH;

    g_timeout_ms = params->timeout_ms == 0U ? 120000U : params->timeout_ms;
    if (g_timeout_ms > INT32_MAX) return NW_DL_ERR_TIMEOUT;
    g_started_ms = HAL_GetTick();

    /* 清理上次残留连接 */
    if (download_close(NW_DL_OK) != NW_DL_OK) return NW_DL_ERR_INTERNAL;

    uint32_t chunk_size = params->chunk_size;
    if (chunk_size == 0 || chunk_size > 1016) chunk_size = 1016;

    /* 解析 URL */
    if (parse_url(params->url, host, sizeof(host), &port, &path, &is_https) != 0)
    {
        printf("[NWDL] URL parse fail\r\n");
        return NW_DL_ERR_URL;
    }
    printf("[NWDL] %s host=%s port=%u\r\n",
           is_https ? "HTTPS" : "HTTP", host, (unsigned)port);

    nw_dl_status_t result = is_https ? fetch_https(params, host, port, path, chunk_size)
                                     : fetch_http(params, host, port, path, chunk_size);
    // 最后一块的存储回调和连接释放也计入总期限，逾期不得返回下载成功。
    return result == NW_DL_OK && download_remaining() == 0U ? NW_DL_ERR_TIMEOUT : result;
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
