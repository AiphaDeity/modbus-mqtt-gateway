/*
 * network_upgrade_downloader.c —— HTTP 分块下载器实现（LwIP netconn）
 *
 * 实现流程：URL解析 → DNS → TCP连接 → HTTP GET → 收响应头 → 循环收body → 回调
 * 每块 ≤1016B（UPGRADE_STAGE_MAX_CHUNK_SIZE），回调里调 UpgradeEntry_Write。
 */
#include "network_upgrade_downloader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lwip/api.h"        /* netconn_* */
#include "lwip/dns.h"        /* netconn_gethostbyname */
#include "lwip/ip_addr.h"

/* ===== 内部缓冲（普通 SRAM，netbuf 数据拷贝出来后 CPU 处理） ===== */
#define NW_DL_RX_BUF_SIZE  1024   /* 略大于 1016，收响应头和 body 复用 */
#define NW_DL_HTTP_PORT    80

/* 解析 URL：http://host[:port]/path → host, port, path
 * 返回 0=成功，-1=格式错误
 * url 例：http://abc.baidubce.com:8080/fw/firmware.gwp
 *       http://abc.baidubce.com/firmware.gwp
 */
static int parse_url(const char *url, char *host, int host_max,
                     uint16_t *port, const char **path)
{
    if (strncmp(url, "http://", 7) != 0) return -1;
    const char *p = url + 7;
    const char *host_start = p;
    /* 找 host 结尾（: / 或 \0） */
    while (*p != ':' && *p != '/' && *p != '\0') p++;
    int host_len = p - host_start;
    if (host_len <= 0 || host_len >= host_max) return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    /* 解析端口 */
    if (*p == ':')
    {
        *port = (uint16_t)atoi(p + 1);
        while (*p != '/' && *p != '\0') p++;
    }
    else
    {
        *port = NW_DL_HTTP_PORT;
    }

    /* path */
    *path = (*p == '/') ? p : "/";
    return 0;
}

/* 从响应头缓冲找 HTTP 状态码 */
static int parse_http_status(const char *hdr, int hdr_len)
{
    /* "HTTP/1.1 200 OK" 或 "HTTP/1.1 206 Partial Content" */
    if (hdr_len < 12) return -1;
    if (strncmp(hdr, "HTTP/", 5) != 0) return -1;
    /* 跳过 HTTP/x.x */
    const char *p = hdr + 5;
    while (*p != ' ' && (p - hdr) < hdr_len) p++;
    while (*p == ' ' && (p - hdr) < hdr_len) p++;
    return atoi(p);
}

/* 从响应头找 Content-Length */
static int32_t parse_content_length(const char *hdr, int hdr_len)
{
    const char *needle = "Content-Length:";
    const char *p = NULL;
    /* 大小写不敏感查找 */
    for (int i = 0; i <= hdr_len - (int)strlen(needle); i++)
    {
        int match = 1;
        for (int j = 0; j < (int)strlen(needle); j++)
        {
            char a = hdr[i + j];
            char b = needle[j];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (a != b) { match = 0; break; }
        }
        if (match) { p = hdr + i + strlen(needle); break; }
    }
    if (!p) return -1;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

nw_dl_status_t NetworkDownloader_Fetch(const nw_dl_params_t *params)
{
    char host[64];
    uint16_t port;
    const char *path;
    ip_addr_t ip;
    err_t err;

    if (!params || !params->url || !params->cb) return NW_DL_ERR_URL;

    uint32_t chunk_size = params->chunk_size;
    if (chunk_size == 0 || chunk_size > 1016) chunk_size = 1016;
    uint32_t timeout_ms = params->timeout_ms;
    if (timeout_ms == 0) timeout_ms = 120000;

    /* 1. 解析 URL */
    if (parse_url(params->url, host, sizeof(host), &port, &path) != 0)
    {
        printf("[NWDL] URL parse fail: %s\r\n", params->url);
        return NW_DL_ERR_URL;
    }
    printf("[NWDL] host=%s port=%u path=%s\r\n", host, (unsigned)port, path);

    /* 2. DNS 解析（同步阻塞） */
    err = netconn_gethostbyname(host, &ip);
    if (err != ERR_OK)
    {
        printf("[NWDL] DNS fail: %s err=%d\r\n", host, (int)err);
        return NW_DL_ERR_DNS;
    }
    printf("[NWDL] DNS OK: %s\r\n", ipaddr_ntoa(&ip));

    /* 3. TCP 连接 */
    struct netconn *conn = netconn_new(NETCONN_TCP);
    if (!conn)
    {
        printf("[NWDL] netconn_new fail\r\n");
        return NW_DL_ERR_CONNECT;
    }
    netconn_set_recvtimeout(conn, 10000);  /* 10s recv 超时 */

    err = netconn_connect(conn, &ip, port);
    if (err != ERR_OK)
    {
        printf("[NWDL] connect fail err=%d\r\n", (int)err);
        netconn_delete(conn);
        return NW_DL_ERR_CONNECT;
    }
    printf("[NWDL] TCP connected\r\n");

    /* 4. 发 HTTP GET 请求 */
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
    err = netconn_write(conn, req, req_len, NETCONN_COPY);
    if (err != ERR_OK)
    {
        printf("[NWDL] write request fail err=%d\r\n", (int)err);
        netconn_delete(conn);
        return NW_DL_ERR_REQUEST;
    }

    /* 5. 收响应头（循环收直到找到 \r\n\r\n） */
    static char hdr_buf[512] __attribute__((section(".ccmram")));   /* 响应头缓冲 */
    int hdr_len = 0;
    int header_end_pos = -1;    /* \r\n\r\n 结束位置（body 起始在 hdr_buf 中的偏移） */
    struct netbuf *nbuf = NULL;

    /* body 残余：如果 header 和 body 在同一个 netbuf，保留 header 后面的 body 数据 */
    static uint8_t body_residual[NW_DL_RX_BUF_SIZE] __attribute__((section(".ccmram")));
    uint32_t body_residual_len = 0;

    while (header_end_pos < 0 && hdr_len < (int)sizeof(hdr_buf) - 1)
    {
        err = netconn_recv(conn, &nbuf);
        if (err != ERR_OK)
        {
            printf("[NWDL] recv header fail err=%d\r\n", (int)err);
            netconn_delete(conn);
            return NW_DL_ERR_RESPONSE;
        }
        void *data;
        uint16_t dlen;
        netbuf_data(nbuf, &data, &dlen);
        int copy = dlen;
        if (hdr_len + copy >= (int)sizeof(hdr_buf)) copy = sizeof(hdr_buf) - 1 - hdr_len;
        memcpy(hdr_buf + hdr_len, data, copy);
        hdr_len += copy;

        /* 找 \r\n\r\n */
        for (int i = 3; i < hdr_len; i++)
        {
            if (hdr_buf[i-3] == '\r' && hdr_buf[i-2] == '\n' &&
                hdr_buf[i-1] == '\r' && hdr_buf[i] == '\n')
            {
                header_end_pos = i + 1;  /* body 起始偏移 */
                break;
            }
        }

        if (header_end_pos >= 0)
        {
            /* header 后面的残余数据是 body 的开头 */
            int residual_start = header_end_pos;  /* body 起始在 hdr_buf 中的偏移 */
            /* 但残余可能不在 hdr_buf 里（如果 copy 被截断了），而是还在 netbuf 里 */
            if (copy < dlen)
            {
                /* netbuf 里还有未拷贝的数据，是 body 的开头 */
                uint16_t remain = dlen - copy;
                if (remain > sizeof(body_residual)) remain = sizeof(body_residual);
                memcpy(body_residual, (uint8_t *)data + copy, remain);
                body_residual_len = remain;
            }
            else if (residual_start < hdr_len)
            {
                /* hdr_buf 里 header 后面还有 body 数据 */
                int remain = hdr_len - residual_start;
                if (remain > (int)sizeof(body_residual)) remain = sizeof(body_residual);
                memcpy(body_residual, hdr_buf + residual_start, remain);
                body_residual_len = (uint32_t)remain;
                hdr_len = residual_start;  /* 截断 hdr_buf 到 header 结束 */
            }
        }

        netbuf_free(nbuf);
        nbuf = NULL;
    }
    hdr_buf[hdr_len] = '\0';

    if (header_end_pos < 0)
    {
        printf("[NWDL] header incomplete\r\n");
        netconn_delete(conn);
        return NW_DL_ERR_RESPONSE;
    }

    /* 6. 解析状态码和 Content-Length */
    int status = parse_http_status(hdr_buf, hdr_len);
    if (status != 200 && status != 206)
    {
        printf("[NWDL] HTTP status %d\r\n", status);
        netconn_delete(conn);
        return NW_DL_ERR_HTTP_STATUS;
    }
    int32_t content_len = parse_content_length(hdr_buf, hdr_len);
    if (content_len < 0)
    {
        printf("[NWDL] no Content-Length\r\n");
        netconn_delete(conn);
        return NW_DL_ERR_NO_CONTENT_LEN;
    }
    if ((uint32_t)content_len != params->total_size)
    {
        printf("[NWDL] size mismatch: got %ld expected %u\r\n",
               (long)content_len, (unsigned)params->total_size);
        netconn_delete(conn);
        return NW_DL_ERR_SIZE_MISMATCH;
    }
    printf("[NWDL] HTTP %d, Content-Length=%ld\r\n", status, (long)content_len);

    /* 7. 循环收 body，每 chunk_size 调回调 */
    static uint8_t chunk_buf[NW_DL_RX_BUF_SIZE] __attribute__((section(".ccmram")));  /* body 分块缓冲 */
    uint32_t offset = 0;
    uint32_t chunk_accum = 0;   /* 当前 chunk 累积字节数 */

    /* 先处理 header 同包带来的 body 残余 */
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
                netconn_delete(conn);
                return NW_DL_ERR_CALLBACK;
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
            if (err == ERR_CLSD || err == ERR_CONN) break;  /* 服务端关连接 */
            printf("[NWDL] recv body fail err=%d at offset=%u\r\n", (int)err, (unsigned)offset);
            netconn_delete(conn);
            return NW_DL_ERR_RECEIVE;
        }

        /* 遍历 netbuf 的所有段（pbuf 链可能有多段） */
        void *seg_data = NULL;
        uint16_t seg_len = 0;

        do {
            /* 每段重新获取数据指针（netbuf_next 后 seg_data/seg_len 失效） */
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

                /* chunk 满或包结束 → 调回调 */
                if (chunk_accum >= chunk_size || offset + chunk_accum >= (uint32_t)content_len)
                {
                    nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum);
                    if (cb_ret != NW_DL_OK)
                    {
                        printf("[NWDL] callback error at offset=%u\r\n", (unsigned)offset);
                        netbuf_free(nbuf);
                        netconn_delete(conn);
                        return NW_DL_ERR_CALLBACK;
                    }
                    offset += chunk_accum;
                    chunk_accum = 0;
                }
            }
        } while (netbuf_next(nbuf) == ERR_OK);  /* 移到下一段，回 do 开头重新调 netbuf_data */

        netbuf_free(nbuf);
        nbuf = NULL;
    }

    /* 处理残余 */
    if (chunk_accum > 0)
    {
        nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum);
        if (cb_ret != NW_DL_OK)
        {
            netconn_delete(conn);
            return NW_DL_ERR_CALLBACK;
        }
        offset += chunk_accum;
    }

    netconn_delete(conn);

    if (offset != (uint32_t)content_len)
    {
        printf("[NWDL] incomplete: got %u expected %ld\r\n", (unsigned)offset, (long)content_len);
        return NW_DL_ERR_RECEIVE;
    }

    printf("[NWDL] download complete: %u bytes\r\n", (unsigned)offset);
    return NW_DL_OK;
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
