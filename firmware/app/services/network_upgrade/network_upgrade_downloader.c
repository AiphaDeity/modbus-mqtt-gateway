/*
 * network_upgrade_downloader.c —— HTTP/HTTPS 分块下载器实现（LwIP netconn + mbedTLS）
 *
 * 实现流程：URL解析 → DNS → TCP连接 → [TLS握手] → HTTP GET → 收响应头 → 循环收body → 回调
 * 每块 ≤1016B（UPGRADE_STAGE_MAX_CHUNK_SIZE），回调里调 UpgradeEntry_Write。
 *
 * 双路径分发：
 *   http://  → 走 netconn 明文路径（原有逻辑，适合内网测试）
 *   https:// → 走 mbedTLS 加密路径（新增，百度云 BOS 生产环境用）
 *
 * 下载流程（HTTP 和 HTTPS 共用逻辑，仅收发层不同）：
 *   1. DNS 解析域名 → IP
 *   2. TCP 连接到服务器（HTTP:80 / HTTPS:443）
 *   3. [HTTPS only] TLS 握手 + 证书校验（GlobalSign Root CA）
 *   4. 发 HTTP GET 请求（含 Authorization 头，如需鉴权）
 *   5. 收响应头，找 "\r\n\r\n" 分隔头和 body
 *   6. 解析状态码（200/206）和 Content-Length
 *   7. 循环收 body，按 chunk_size 分块回调
 *   8. 回调里调 UpgradeEntry_Write 写 Flash
 *
 * 内存布局：所有收发缓冲放 CCMRAM，不占普通 SRAM
 */
#include "network_upgrade_downloader.h" // 本模块对外接口声明

#include <stdio.h> // 标准IO，printf打日志
#include <string.h> // 字符串操作，strncmp/memcpy
#include <stdlib.h> // 标准库，atoi转数字

#include "lwip/api.h"        // LwIP netconn接口，TCP连接收发
#include "lwip/dns.h"        // DNS域名解析
#include "lwip/ip_addr.h"    // IP地址结构

// HTTPS加密支持
#include "https_tls.h" // TLS会话封装，HTTPS收发
#include "https_ca_cert.h"   // 百度云BOS的根证书，验证服务器身份

// 接收缓冲大小，略大于1016，响应头和body复用
#define NW_DL_RX_BUF_SIZE  1024 // 接收缓冲1024字节
// HTTP默认端口
#define NW_DL_HTTP_PORT    80 // HTTP明文端口80
// HTTPS默认端口
#define NW_DL_HTTPS_PORT   443 // HTTPS加密端口443

// 全局TCP连接，HTTP明文路径用，HTTPS由TLS会话管理
static struct netconn *g_download_conn; // 当前下载用的TCP连接句柄
// TLS会话结构，加密路径用
static https_tls_session_t g_tls_session; // TLS会话上下文
// 标记这次下载是不是走TLS加密
static bool g_using_tls; // 是不是在用TLS加密通道

// 函数：download_close —— 统一收尾关连接
// 功能：下载结束（成功或失败都走这）统一清理连接：HTTPS模式先调mbedtls关TLS会话，然后HTTP/HTTPS都删底层TCP netconn；把传入的结果码原样返回，方便各退出路径一行完成收尾
// 参数：result —— 要透传的最终状态码（成功OK或各错误码），函数不改它
// 返回：nw_dl_status_t，原样返回传入的result，同时连接已全部释放
static nw_dl_status_t download_close(nw_dl_status_t result) // 统一收尾函数：关TLS或删TCP连接再返回错误码
{
    if (g_using_tls) // 走TLS分支关会话
    {
        HttpsTls_Close(&g_tls_session); // 关TLS会话
        g_using_tls = false; // 清TLS标志
        return result; // 原样返回结果
    }

    err_t err = netconn_delete(g_download_conn); // 走TCP分支删连接
    if (err == ERR_OK) // 删除成功判断
        g_download_conn = NULL; // 句柄置空
    else // 删除失败走else
    {
        printf("[NWDL] connection delete fail err=%d\r\n", (int)err); // 打印删除失败
        if (result == NW_DL_OK) // 原结果是成功的话
            result = NW_DL_ERR_INTERNAL; // 改成内部错误
    }
    return result; // 返回最终结果
}

// 函数：parse_url —— URL解析
// 功能：把https://host:port/path格式的下载URL手工拆解：判断http还是https、剥掉协议头、分离主机名和可选端口（没有端口用默认80/443）、剩余当路径；不考虑嵌套复杂URL，够拉BOS固件用
// 参数：url —— 原始URL字符串；host —— 出参，主机名拷到这里；host_max —— host缓冲容量防越界；port —— 出参，端口号（默认80或443）；path —— 出参，路径指针（指向url内部，不用拷贝）；is_https —— 出参，真表示要走TLS
// 返回：int，0代表解析成功；-1代表格式不对（没有协议头或主机名超长）
static int parse_url(const char *url, char *host, int host_max, // URL解析函数，拆主机端口路径
                     uint16_t *port, const char **path, bool *is_https) // 签名续行：出参指针集合
{
    *is_https = false; // 先默认不是HTTPS

    // 先看协议前缀，确定默认端口
    if (strncmp(url, "https://", 8) == 0)
    {
        *is_https = true; // 标记HTTPS
        *port = NW_DL_HTTPS_PORT;   // 443
    }
    else if (strncmp(url, "http://", 7) == 0)
    {
        *is_https = false; // 标记HTTP
        *port = NW_DL_HTTP_PORT;    // 80
    }
    else // 都不是走else
    {
        return -1;   // 不认识的协议
    }

    // 跳过协议前缀，指向主机名开头
    const char *p = url + (*is_https ? 8 : 7); // 跳过协议前缀
    const char *host_start = p; // 记主机名起点
    // 主机名到冒号、斜杠或者字符串结束就停
    while (*p != ':' && *p != '/' && *p != '\0') p++; // 扫到冒号斜杠或结尾
    int host_len = p - host_start; // 算主机名长度
    if (host_len <= 0 || host_len >= host_max) return -1; // 空或超长返回失败
    memcpy(host, host_start, host_len); // 复制主机名
    host[host_len] = '\0'; // 补结束符

    // 有冒号就是指定了端口，转成数字
    if (*p == ':') // 有冒号就是显式端口
    {
        *port = (uint16_t)atoi(p + 1); // 冒号后转端口号
        while (*p != '/' && *p != '\0') p++;   // 跳过端口数字
    }

    // 路径，有斜杠就从斜杠开始，没有就用根路径
    *path = (*p == '/') ? p : "/"; // 路径取斜杠处或根路径
    return 0; // 返回成功
}

// 函数：parse_http_status —— 解析HTTP状态码
// 功能：在HTTP响应头里找"HTTP/1.x 200"这种状态行，把三位状态码解析成整数：200完整下载、206分块续传是正常的，其他状态（403没权限、404没文件）都当失败处理；响应头残缺返回-1
// 参数：hdr —— 响应头缓冲指针；hdr_len —— 响应头字节数
// 返回：int，解析出的状态码（200/206等）；-1代表响应头格式错误没找到状态码
static int parse_http_status(const char *hdr, int hdr_len) // 解析状态码函数
{
    if (hdr_len < 12) return -1; // 头太短返回失败
    if (strncmp(hdr, "HTTP/", 5) != 0) return -1; // 不是HTTP开头返回失败
    // 跳过HTTP/版本部分
    const char *p = hdr + 5; // 跳过版本号
    while (*p != ' ' && (p - hdr) < hdr_len) p++; // 扫到空格
    // 跳过空格，指向状态码数字
    while (*p == ' ' && (p - hdr) < hdr_len) p++; // 跳过空格
    return atoi(p); // 状态码转数字返回
}

// 函数：parse_content_length —— 解析Content-Length
// 功能：在HTTP响应头里找Content-Length字段拿文件总大小：手写大小写不敏感匹配（LwIP没有strcasestr）逐行扫，找到后把数字解析成int32；升级模块靠它知道固件总字节数来算进度和对齐flash
// 参数：hdr —— 响应头缓冲指针；hdr_len —— 响应头字节数
// 返回：int32_t，解析出的内容长度字节数；-1代表响应头里没带Content-Length
static int32_t parse_content_length(const char *hdr, int hdr_len) // 解析Content-Length函数
{
    const char *needle = "Content-Length:"; // 要找的字段名
    const char *p = NULL; // 匹配位置指针
    int needle_len = (int)strlen(needle); // 字段名长度
    // 逐个位置试匹配
    for (int i = 0; i <= hdr_len - needle_len; i++) // 逐个位置尝试循环
    {
        int match = 1; // 先假设匹配
        for (int j = 0; j < needle_len; j++) // 逐字符比较循环
        {
            char a = hdr[i + j]; // 头里的字符
            char b = needle[j]; // 目标字符
// 函数：fetch_https —— HTTPS加密下载路径
// 功能：TLS加密下载全流程：DNS解析域名→建TCP连接到443→mbedtls做TLS握手建加密通道→发HTTP GET请求→收响应头（验状态码、取总大小）→循环收body：每攒到chunk_size字节就调上层回调写flash，直到收满Content-Length；全程带超时保护，任何一步失败都走download_close统一收尾
// 参数：params —— 下载参数结构（回调函数指针等全套）；host —— 服务器主机名字符串；port —— 端口号（一般443）；path —— 请求路径（BOS对象地址）；chunk_size —— 每块回调的字节数；timeout_ms —— 每步网络操作的超时毫秒数
// 返回：nw_dl_status_t，NW_DL_OK代表全部字节收完；错误码代表DNS/连接/TLS握手/超时/状态码不对等中途失败
            if (a >= 'a' && a <= 'z') a -= 32;   // 小写转大写再比较
            if (a != b) { match = 0; break; } // 不等就标记失配
        }
        if (match) { p = hdr + i + needle_len; break; } // 全等就记位置跳出
    }
    if (!p) return -1; // 没找到返回-1
    while (*p == ' ' || *p == '\t') p++;   // 跳过空格制表符
    return atoi(p);                        // 转成数字
}

// HTTPS加密下载路径，DNS、TCP、TLS握手、GET、收头、收body、回调，比HTTP多TLS
static nw_dl_status_t fetch_https(const nw_dl_params_t *params, // HTTPS下载主函数
                                  const char *host, uint16_t port, // 签名续行：主机端口
                                  const char *path, uint32_t chunk_size, // 签名续行：路径块大小
                                  uint32_t timeout_ms) // 签名续行：超时毫秒
{
    ip_addr_t ip; // 解析出的IP地址
    err_t err; // LwIP错误码

    // 第一步DNS解析域名
    err = netconn_gethostbyname(host, &ip); // DNS解析域名
    if (err != ERR_OK) // 解析失败判断
    {
        printf("[NWDL] DNS fail: %s err=%d\r\n", host, (int)err); // 打印DNS失败
        return NW_DL_ERR_DNS; // 返回DNS错误
    }
    printf("[NWDL] DNS OK: %s\r\n", ipaddr_ntoa(&ip)); // 打印解析到的IP

    // 第二步建TCP连接
    struct netconn *conn = netconn_new(NETCONN_TCP); // 新建TCP连接
    if (!conn) // 创建失败判断
    {
        printf("[NWDL] netconn_new fail\r\n"); // 打印创建失败
        return NW_DL_ERR_CONNECT; // 返回连接错误
    }
    g_download_conn = conn; // 存到全局句柄
    netconn_set_recvtimeout(conn, 10000); // 收超时10秒

    err = netconn_connect(conn, &ip, port); // 连接服务器
    if (err != ERR_OK) // 连接失败判断
    {
        printf("[NWDL] connect fail err=%d\r\n", (int)err); // 打印连接失败
        nw_dl_status_t r = NW_DL_ERR_CONNECT; // 返回连接错误
        netconn_delete(conn); // 删除连接
        g_download_conn = NULL; // 句柄置空
        return r; // 返回错误
    }
    printf("[NWDL] TCP connected (HTTPS)\r\n"); // 打印TCP已连

    // 第三步TLS握手，用预置根证书验证服务器，验证失败就拒绝，防中间人攻击
    https_tls_result_t tls_ret = HttpsTls_Connect(&g_tls_session, conn, // TLS握手，带证书校验
                                                    host, // 签名续行：主机名
                                                    g_https_ca_cert_der, // 签名续行：CA证书
                                                    g_https_ca_cert_der_len, // 签名续行：证书长度
                                                    timeout_ms); // 签名续行：超时
    if (tls_ret != HTTPS_TLS_OK) // 握手失败判断
    {
        printf("[NWDL] TLS handshake fail: %d\r\n", (int)tls_ret); // 打印握手失败
        HttpsTls_Close(&g_tls_session); // 关TLS会话
        g_download_conn = NULL; // 句柄置空
        return NW_DL_ERR_CONNECT; // 返回连接错误
    }
    g_using_tls = true; // 标记在用TLS
    printf("[NWDL] TLS connected\r\n"); // 打印TLS已连

    // 第四步发HTTP GET请求，经TLS加密，有token就带Authorization头
    char req[256]; // 请求缓冲区
    int req_len; // 请求长度
    if (params->auth_token && params->auth_token[0]) // 有鉴权token判断
    {
        req_len = snprintf(req, sizeof(req), // 拼带Authorization的GET
            "GET %s HTTP/1.1\r\nHost: %s\r\nAuthorization: %s\r\nConnection: close\r\n\r\n", // 请求行主机头鉴权头关连接
            path, host, params->auth_token); // printf续行：路径主机token
    }
    else // 没token走else
    {
        req_len = snprintf(req, sizeof(req), // 拼普通GET
            "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", // 请求行主机头关连接
            path, host); // printf续行：路径主机
    }
    if (req_len <= 0 || (size_t)req_len >= sizeof(req)) // 请求超长判断
        return download_close(NW_DL_ERR_REQUEST); // 收尾返回请求错误

    int sent = HttpsTls_Send(&g_tls_session, (const uint8_t *)req, (size_t)req_len); // TLS发送请求
    if (sent <= 0) // 发送失败判断
    {
        printf("[NWDL] TLS send request fail\r\n"); // 打印发送失败
        return download_close(NW_DL_ERR_REQUEST); // 收尾返回请求错误
    }

    // 第五步收响应头，找连续两个换行，那是HTTP头结束的标志，后面就是body
    static char hdr_buf[512] __attribute__((section(".bss.ccmram"))); // 响应头缓冲，CCMRAM
    int hdr_len = 0; // 已收头长度
    int header_end_pos = -1; // 头尾分隔位置

    while (header_end_pos < 0 && hdr_len < (int)sizeof(hdr_buf) - 1) // 收头循环，没找到分隔且没满
    {
        int got = HttpsTls_Recv(&g_tls_session, (uint8_t *)hdr_buf + hdr_len, // TLS收一段数据
                                 (size_t)(sizeof(hdr_buf) - 1 - hdr_len)); // 签名续行：收进头缓冲尾部
        if (got < 0) // 签名续行：剩余空间
        {
            printf("[NWDL] TLS recv header fail\r\n"); // 接收出错判断
            return download_close(NW_DL_ERR_RESPONSE); // 收尾返回响应错误
        }
        if (got == 0) // 连接关闭判断
        {
            printf("[NWDL] TLS EOF during header\r\n"); // 打印头没收完就断
            return download_close(NW_DL_ERR_RESPONSE); // 收尾返回响应错误
        }
        hdr_len += got; // 累加已收长度

        // 逐个字符找连续的回车换行回车换行，找到就是头结束了
        for (int i = 3; i < hdr_len; i++) // 在已收数据里找rnrn
        {
            if (hdr_buf[i-3] == '\r' && hdr_buf[i-2] == '\n' && // 连续四个字符判断
                hdr_buf[i-1] == '\r' && hdr_buf[i] == '\n') // rn rn就是头结束
            {
                header_end_pos = i + 1; // 记头结束位置
                break; // 跳出查找循环
            }
        }
    }
    hdr_buf[hdr_len] = '\0'; // 补字符串结束符

    if (header_end_pos < 0) // 没找到头结束判断
    {
        printf("[NWDL] header incomplete\r\n"); // 打印头不完整
        return download_close(NW_DL_ERR_RESPONSE); // 收尾返回响应错误
    }

    // 第六步解析状态码和内容长度，只接受200和206
    int status = parse_http_status(hdr_buf, hdr_len); // 解析状态码
    if (status != 200 && status != 206) // 200或206之外判断
    {
        printf("[NWDL] HTTP status %d\r\n", status); // 打印状态码
        return download_close(NW_DL_ERR_HTTP_STATUS); // 收尾返回状态码错误
    }
    int32_t content_len = parse_content_length(hdr_buf, hdr_len); // 解析Content-Length
    if (content_len < 0) // 没找到判断
    {
        printf("[NWDL] no Content-Length\r\n"); // 打印没长度字段
        return download_close(NW_DL_ERR_NO_CONTENT_LEN); // 收尾返回缺长度错误
    }
    // 服务器返回的大小必须和通知里的总大小一致，防止下载错文件
    if ((uint32_t)content_len != params->total_size) // 长度和预期不符判断
    {
        printf("[NWDL] size mismatch: got %ld expected %u\r\n", // 打印大小不匹配
               (long)content_len, (unsigned)params->total_size); // printf续行：实际值期望值
        return download_close(NW_DL_ERR_SIZE_MISMATCH); // 收尾返回大小不匹配
    }
    printf("[NWDL] HTTPS %d, Content-Length=%ld\r\n", status, (long)content_len); // 打印状态和长度

    // 第七步收body，先处理头同包带的残余body，一个包可能同时有头和body开头
    static uint8_t chunk_buf[NW_DL_RX_BUF_SIZE] __attribute__((section(".bss.ccmram"))); // 收body的块缓冲，CCMRAM
    uint32_t offset = 0;       // 已经回调写Flash的字节偏移
    uint32_t chunk_accum = 0;  // 块缓冲里已经攒了多少字节

    // 算头后面带了多少body数据
    int body_in_hdr = hdr_len - header_end_pos; // 头里夹带的body字节数
    if (body_in_hdr > 0) // 有夹带判断
    {
        uint32_t copy = (uint32_t)body_in_hdr; // 先按夹带数准备
        if (copy > chunk_size) copy = chunk_size; // 超过块大小就截断
        memcpy(chunk_buf, hdr_buf + header_end_pos, copy); // 拷进块缓冲
        chunk_accum = copy; // 记已积累量
        // 攒够一块或者到文件末尾就回调写Flash
        if (chunk_accum >= chunk_size || offset + chunk_accum >= (uint32_t)content_len) // 攒满一块或到末尾判断
        {
            nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum); // 交回调写Flash
            if (cb_ret != NW_DL_OK) // 回调失败判断
            {
                printf("[NWDL] callback error at residual offset=%u\r\n", (unsigned)offset); // 打印回调错误
                return download_close(NW_DL_ERR_CALLBACK); // 收尾返回回调错误
            }
            offset += chunk_accum; // 偏移前移
            chunk_accum = 0; // 清积累量
        }
    }

    // 第八步循环收剩下的body，攒够一块就回调
    while (offset < (uint32_t)content_len) // 没收完继续收循环
    {
        uint32_t space = chunk_size - chunk_accum; // 算块缓冲剩余空间
        int got = HttpsTls_Recv(&g_tls_session, chunk_buf + chunk_accum, space); // TLS收一段body
        if (got < 0) // 接收出错判断
        {
            printf("[NWDL] TLS recv body fail at offset=%u\r\n", (unsigned)offset); // 打印接收失败
            return download_close(NW_DL_ERR_RECEIVE); // 收尾返回接收错误
        }
        if (got == 0) break;  // 服务器关连接了，EOF

        chunk_accum += (uint32_t)got; // 累加已收量
        // 攒够一块或者到末尾就回调
        if (chunk_accum >= chunk_size || offset + chunk_accum >= (uint32_t)content_len) // 攒满一块或到末尾判断
        {
            nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum); // 交回调写Flash
            if (cb_ret != NW_DL_OK) // 回调失败判断
            {
                printf("[NWDL] callback error at offset=%u\r\n", (unsigned)offset); // 打印回调错误
                return download_close(NW_DL_ERR_CALLBACK); // 收尾返回回调错误
            }
            offset += chunk_accum; // 偏移前移
            chunk_accum = 0; // 清积累量
        }
    }

    // 最后一块不够一块大小也要回调，不能丢
    if (chunk_accum > 0) // 收完还剩尾巴判断
    {
        nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum); // 交回调写Flash
        if (cb_ret != NW_DL_OK) // 回调失败判断
            return download_close(NW_DL_ERR_CALLBACK); // 收尾返回回调错误
        offset += chunk_accum; // 偏移前移
    }

    // 校验完整性，收到的总字节必须正好等于Content-Length，少了就是下载不完整
    if (offset != (uint32_t)content_len) // 总数不符判断
    {
// 函数：fetch_http —— HTTP明文下载路径
// 功能：明文HTTP下载，流程和HTTPS版完全一样只是没有TLS层：DNS解析→TCP连接→发GET→收响应头→循环收body调回调写flash；收发直接用LwIP的netconn不加密，适合内网测试环境用
// 参数：params —— 下载参数结构（回调函数指针等全套）；host —— 服务器主机名字符串；port —— 端口号（一般80）；path —— 请求路径；chunk_size —— 每块回调的字节数；timeout_ms —— 每步网络操作的超时毫秒数
// 返回：nw_dl_status_t，NW_DL_OK代表全部字节收完；错误码代表DNS/连接/超时/状态码不对等中途失败
        printf("[NWDL] incomplete: got %u expected %ld\r\n", (unsigned)offset, (long)content_len); // 打印不完整
        return download_close(NW_DL_ERR_RECEIVE); // 收尾返回接收错误
    }

    nw_dl_status_t result = download_close(NW_DL_OK); // 正常收尾关连接
    if (result != NW_DL_OK) return result; // 收尾出错就透传
    printf("[NWDL] download complete: %u bytes\r\n", (unsigned)offset); // 打印下载完成
    return NW_DL_OK; // 返回成功
}

// HTTP明文下载路径，和HTTPS流程一样，只是没有TLS，收发用netconn，内网测试用
static nw_dl_status_t fetch_http(const nw_dl_params_t *params, // HTTP明文下载主函数
                                 const char *host, uint16_t port, // 签名续行：主机端口
                                 const char *path, uint32_t chunk_size, // 签名续行：路径块大小
                                 uint32_t timeout_ms) // 签名续行：超时毫秒
{
    ip_addr_t ip; // 解析出的IP地址
    err_t err; // LwIP错误码

    // 第一步DNS解析
    err = netconn_gethostbyname(host, &ip); // DNS解析域名
    if (err != ERR_OK) // 解析失败判断
    {
        printf("[NWDL] DNS fail: %s err=%d\r\n", host, (int)err); // 打印DNS失败
        return NW_DL_ERR_DNS; // 返回DNS错误
    }
    printf("[NWDL] DNS OK: %s\r\n", ipaddr_ntoa(&ip)); // 打印解析到的IP

    // 第二步建TCP连接
    struct netconn *conn = netconn_new(NETCONN_TCP); // 新建TCP连接
    if (!conn) // 创建失败判断
    {
        printf("[NWDL] netconn_new fail\r\n"); // 打印创建失败
        return NW_DL_ERR_CONNECT; // 返回连接错误
    }
    g_download_conn = conn; // 存到全局句柄
    netconn_set_recvtimeout(conn, 10000); // 收超时10秒

    err = netconn_connect(conn, &ip, port); // 连接服务器
    if (err != ERR_OK) // 连接失败判断
    {
        printf("[NWDL] connect fail err=%d\r\n", (int)err); // 打印连接失败
        return download_close(NW_DL_ERR_CONNECT); // 收尾返回连接错误
    }
    printf("[NWDL] TCP connected\r\n"); // 打印TCP已连

    // 第三步发明文HTTP GET请求，有token就带
    char req[256]; // 请求缓冲区
    int req_len; // 请求长度
    if (params->auth_token && params->auth_token[0]) // 有鉴权token判断
    {
        req_len = snprintf(req, sizeof(req), // 拼带Authorization的GET
            "GET %s HTTP/1.1\r\nHost: %s\r\nAuthorization: %s\r\nConnection: close\r\n\r\n", // 请求行主机头鉴权头关连接
            path, host, params->auth_token); // printf续行：路径主机token
    }
    else // 没token走else
    {
        req_len = snprintf(req, sizeof(req), // 拼普通GET
            "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", // 请求行主机头关连接
            path, host); // printf续行：路径主机
    }
    if (req_len <= 0 || (size_t)req_len >= sizeof(req)) // 请求超长判断
        return download_close(NW_DL_ERR_REQUEST); // 收尾返回请求错误
    err = netconn_write(conn, req, req_len, NETCONN_COPY); // TCP发送请求
    if (err != ERR_OK) // 发送失败判断
    {
        printf("[NWDL] write request fail err=%d\r\n", (int)err); // 打印发送失败
        return download_close(NW_DL_ERR_REQUEST); // 收尾返回请求错误
    }

    // 第四步收响应头，netbuf可能有多个分片，要逐段拼接找头结束，同包带的body存残余
    static char hdr_buf[512] __attribute__((section(".bss.ccmram"))); // 响应头缓冲，CCMRAM
    int hdr_len = 0; // 已收头长度
    int header_end_pos = -1; // 头尾分隔位置
    struct netbuf *nbuf = NULL; // netbuf指针

    // 收头时带的body数据存这里，后面接着处理
    static uint8_t body_residual[NW_DL_RX_BUF_SIZE] __attribute__((section(".bss.ccmram"))); // 夹带body缓冲，CCMRAM
    uint32_t body_residual_len = 0; // 夹带body长度

    while (header_end_pos < 0 && hdr_len < (int)sizeof(hdr_buf) - 1) // 收头循环
    {
        err = netconn_recv(conn, &nbuf); // 收一个netbuf
        if (err != ERR_OK) // 接收出错判断
        {
            printf("[NWDL] recv header fail err=%d\r\n", (int)err); // 打印收头失败
            return download_close(NW_DL_ERR_RESPONSE); // 收尾返回响应错误
        }
        void *data; // 数据指针
        uint16_t dlen; // 数据长度
        netbuf_data(nbuf, &data, &dlen); // 取netbuf数据指针和长度
        int copy = dlen; // 本轮要拷贝量
        if (hdr_len + copy >= (int)sizeof(hdr_buf)) copy = sizeof(hdr_buf) - 1 - hdr_len; // 截断到头缓冲容量
        memcpy(hdr_buf + hdr_len, data, copy); // 拷进头缓冲
        hdr_len += copy; // 累加已收长度

        // 找头结束的连续换行
        for (int i = 3; i < hdr_len; i++) // 在已收数据里找rnrn
        {
            if (hdr_buf[i-3] == '\r' && hdr_buf[i-2] == '\n' && // 连续四个字符判断
                hdr_buf[i-1] == '\r' && hdr_buf[i] == '\n') // rn rn就是头结束
            {
                header_end_pos = i + 1; // 记头结束位置
                break; // 跳出查找循环
            }
        }

        if (header_end_pos >= 0) // 已找到头结束判断
        {
            int residual_start = header_end_pos; // 头结束位置
            // 数据没全拷到头缓冲，剩下的就是body残余
            if (copy < dlen) // netbuf里还有没拷的判断
            {
                uint16_t remain = dlen - copy; // 剩余量
                if (remain > sizeof(body_residual)) remain = sizeof(body_residual); // 截断到缓冲容量
                memcpy(body_residual, (uint8_t *)data + copy, remain); // 拷进夹带缓冲
                body_residual_len = remain; // 记夹带长度
            }
            // 全拷了但头结束位置在中间，后面的是残余
            else if (residual_start < hdr_len) // 头缓冲里有残余判断
            {
                int remain = hdr_len - residual_start; // 残余量
                if (remain > (int)sizeof(body_residual)) remain = sizeof(body_residual); // 截断到缓冲容量
                memcpy(body_residual, hdr_buf + residual_start, remain); // 拷进夹带缓冲
                body_residual_len = (uint32_t)remain; // 记夹带长度
                hdr_len = residual_start; // 头长度截到头结束
            }
        }

        netbuf_delete(nbuf); // 释放netbuf
        nbuf = NULL; // 指针置空
    }
    hdr_buf[hdr_len] = '\0'; // 补字符串结束符

    if (header_end_pos < 0) // 没找到头结束判断
    {
        printf("[NWDL] header incomplete\r\n"); // 打印头不完整
        return download_close(NW_DL_ERR_RESPONSE); // 收尾返回响应错误
    }

    // 解析状态码，只接受200和206
    int status = parse_http_status(hdr_buf, hdr_len); // 解析状态码
    if (status != 200 && status != 206) // 200或206之外判断
    {
        printf("[NWDL] HTTP status %d\r\n", status); // 打印状态码
        return download_close(NW_DL_ERR_HTTP_STATUS); // 收尾返回状态码错误
    }
    // 解析Content-Length，没有就报错
    int32_t content_len = parse_content_length(hdr_buf, hdr_len); // 解析Content-Length
    if (content_len < 0) // 没找到判断
    {
        printf("[NWDL] no Content-Length\r\n"); // 打印没长度字段
        return download_close(NW_DL_ERR_NO_CONTENT_LEN); // 收尾返回缺长度错误
    }
    // 大小必须和通知里的一致
    if ((uint32_t)content_len != params->total_size) // 长度和预期不符判断
    {
        printf("[NWDL] size mismatch: got %ld expected %u\r\n", // 打印大小不匹配
               (long)content_len, (unsigned)params->total_size); // printf续行：实际值期望值
        return download_close(NW_DL_ERR_SIZE_MISMATCH); // 收尾返回大小不匹配
    }
    printf("[NWDL] HTTP %d, Content-Length=%ld\r\n", status, (long)content_len); // 打印状态和长度

    // 开始收body，块缓冲放CCMRAM
    static uint8_t chunk_buf[NW_DL_RX_BUF_SIZE] __attribute__((section(".bss.ccmram"))); // 收body的块缓冲，CCMRAM
    uint32_t offset = 0; // 已下载偏移
    uint32_t chunk_accum = 0; // 块内积累量

    // 先处理头同包带的残余body
    if (body_residual_len > 0) // 有夹带body判断
    {
        uint32_t copy = body_residual_len; // 先按夹带数准备
        if (copy > chunk_size) copy = chunk_size; // 超过块大小就截断
        memcpy(chunk_buf, body_residual, copy); // 拷进块缓冲
        chunk_accum = copy; // 记已积累量
        if (chunk_accum >= chunk_size || offset + chunk_accum >= (uint32_t)content_len) // 攒满一块或到末尾判断
        {
            nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum); // 交回调写Flash
            if (cb_ret != NW_DL_OK) // 回调失败判断
            {
                printf("[NWDL] callback error at residual offset=%u\r\n", (unsigned)offset); // 打印回调错误
                return download_close(NW_DL_ERR_CALLBACK); // 收尾返回回调错误
            }
            offset += chunk_accum; // 偏移前移
            chunk_accum = 0; // 清积累量
        }
    }

    // 循环收剩下的body
    while (offset < (uint32_t)content_len) // 没收完继续收循环
    {
        err = netconn_recv(conn, &nbuf); // 收一个netbuf
        if (err != ERR_OK) // 接收出错判断
        {
            if (err == ERR_CLSD || err == ERR_CONN) break;  // 服务器关了，正常结束
            printf("[NWDL] recv body fail err=%d at offset=%u\r\n", (int)err, (unsigned)offset); // 打印收body失败
            return download_close(NW_DL_ERR_RECEIVE); // 收尾返回接收错误
        }

        // netbuf可能有多个分片，逐个分片处理
        void *seg_data = NULL; // 段数据指针
        uint16_t seg_len = 0; // 段长度

        do { // 处理netbuf所有链段
            netbuf_data(nbuf, &seg_data, &seg_len); // 取段数据和长度
            uint16_t seg_consumed = 0; // 段内已消费量
            // 分片里的数据逐个字节拷到块缓冲，攒够就回调
            while (seg_consumed < seg_len && offset + chunk_accum < (uint32_t)content_len) // 没消费完且没下完循环
            {
                uint16_t avail = seg_len - seg_consumed; // 段内剩余量
                uint32_t space = chunk_size - chunk_accum; // 块内剩余空间
                uint32_t copy = (avail < space) ? avail : space; // 这轮拷贝量取小

                memcpy(chunk_buf + chunk_accum, (uint8_t *)seg_data + seg_consumed, copy); // 拷进块缓冲
                chunk_accum += copy; // 累加积累量
                seg_consumed += (uint16_t)copy; // 消费段内量

                // 攒够一块或者到文件末尾就回调
                if (chunk_accum >= chunk_size || offset + chunk_accum >= (uint32_t)content_len) // 攒满一块或到末尾判断
                {
                    nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum); // 交回调写Flash
                    if (cb_ret != NW_DL_OK) // 回调失败判断
                    {
                        printf("[NWDL] callback error at offset=%u\r\n", (unsigned)offset); // 打印回调错误
                        netbuf_delete(nbuf); // 释放netbuf
                        return download_close(NW_DL_ERR_CALLBACK); // 收尾返回回调错误
                    }
                    offset += chunk_accum; // 偏移前移
                    chunk_accum = 0; // 清积累量
                }
            }
        } while (netbuf_next(nbuf) == ERR_OK);  // 看下一个分片

        netbuf_delete(nbuf); // 释放netbuf
        nbuf = NULL; // 指针置空
    }

    // 最后一块不足也要回调
    if (chunk_accum > 0) // 收完还剩尾巴判断
    {
        nw_dl_status_t cb_ret = params->cb(params->ctx, offset, chunk_buf, chunk_accum); // 交回调写Flash
        if (cb_ret != NW_DL_OK) // 回调失败判断
            return download_close(NW_DL_ERR_CALLBACK); // 收尾返回回调错误
        offset += chunk_accum; // 偏移前移
    }

    // 完整性校验，大小不对就是下载不完整
    if (offset != (uint32_t)content_len) // 总数不符判断
    {
        printf("[NWDL] incomplete: got %u expected %ld\r\n", (unsigned)offset, (long)content_len); // 打印不完整
        return download_close(NW_DL_ERR_RECEIVE); // 收尾返回接收错误
    }

    nw_dl_status_t result = download_close(NW_DL_OK); // 正常收尾关连接
    if (result != NW_DL_OK) return result; // 收尾出错就透传
    printf("[NWDL] download complete: %u bytes\r\n", (unsigned)offset); // 打印下载完成
    return NW_DL_OK; // 返回成功
}

// 函数：NetworkDownloader_Fetch —— 下载固件总入口（对外接口）
// 功能：升级模块拉固件的总入口：先调parse_url拆URL，按协议分发——https走fetch_https（TLS握手+加密下载），http走fetch_http（裸TCP明文下载）；两条路径都是流式接收，每块数据通过回调交给上层写flash，不整包缓存；全流程结束统一download_close收尾
// 参数：params —— 下载参数结构指针：URL、每块回调函数指针、回调上下文、超时等全套
// 返回：nw_dl_status_t，NW_DL_OK代表全部字节下载完；错误码代表URL错/连接失败/超时/状态码不对等中途失败
nw_dl_status_t NetworkDownloader_Fetch(const nw_dl_params_t *params) // 对外总入口：解析URL按协议分发
{
    char host[64]; // 主机名缓冲
    uint16_t port; // 端口变量
    const char *path; // 路径指针
    bool is_https; // 是否HTTPS

    if (!params || !params->url || !params->cb) return NW_DL_ERR_URL; // 参数不完整判断

    // 先清理上次残留的连接，防止连接泄漏
    if (download_close(NW_DL_OK) != NW_DL_OK) return NW_DL_ERR_INTERNAL; // 清残留连接判断

    // 块大小限制，0或者超过1016就用1016，Flash写入单元最大1016
    uint32_t chunk_size = params->chunk_size; // 取块大小参数
    if (chunk_size == 0 || chunk_size > 1016) chunk_size = 1016; // 0或超1016就用默认
    uint32_t timeout_ms = params->timeout_ms; // 取超时参数
    if (timeout_ms == 0) timeout_ms = 120000;   // 默认2分钟超时

    // 解析URL，拆出协议、主机、端口、路径
    if (parse_url(params->url, host, sizeof(host), &port, &path, &is_https) != 0) // 解析URL判断
    {
        printf("[NWDL] URL parse fail: %s\r\n", params->url); // 打印解析失败
        return NW_DL_ERR_URL; // 返回URL错误
    }
    printf("[NWDL] %s host=%s port=%u path=%s\r\n", // 打印协议主机端口路径
           is_https ? "HTTPS" : "HTTP", host, (unsigned)port, path); // printf续行：四项内容

    // 按协议分发到对应路径
    if (is_https) // HTTPS分支判断
    {
        return fetch_https(params, host, port, path, chunk_size, timeout_ms); // 走加密下载
    }
    else // 否则走明文
    {
        return fetch_http(params, host, port, path, chunk_size, timeout_ms); // 走明文下载
    }
}

// 函数：NetworkDownloader_ErrorString —— 错误码转字符串（对外接口）
// 功能：把下载器的状态码枚举翻译成可读英文短语（ok/url_invalid/timeout/connect_failed等），查表式映射；日志和云端状态上报都用它，让人一眼看懂失败在哪一环
// 参数：status —— 下载状态码枚举值
// 返回：const char*，状态对应的字符串指针；不认识的码返回"unknown"
const char *NetworkDownloader_ErrorString(nw_dl_status_t status) // 错误码转字符串函数
{
    switch (status) // 按错误码分发
    {
        case NW_DL_OK:                 return "ok"; // 成功
        case NW_DL_ERR_URL:            return "E_URL"; // URL错误
        case NW_DL_ERR_DNS:            return "E_DNS"; // DNS错误
        case NW_DL_ERR_CONNECT:        return "E_CONNECT"; // 连接错误
        case NW_DL_ERR_REQUEST:        return "E_REQUEST"; // 请求错误
        case NW_DL_ERR_RESPONSE:       return "E_RESPONSE"; // 响应错误
        case NW_DL_ERR_HTTP_STATUS:    return "E_HTTP_STATUS"; // HTTP状态码错误
        case NW_DL_ERR_NO_CONTENT_LEN: return "E_NO_CONTENT_LEN"; // 缺Content-Length
        case NW_DL_ERR_SIZE_MISMATCH:  return "E_SIZE_MISMATCH"; // 大小不匹配
        case NW_DL_ERR_RECEIVE:        return "E_RECEIVE"; // 接收错误
        case NW_DL_ERR_CALLBACK:       return "E_CALLBACK"; // 回调错误
        case NW_DL_ERR_TIMEOUT:        return "E_TIMEOUT"; // 超时错误
        case NW_DL_ERR_INTERNAL:       return "E_INTERNAL"; // 内部错误
        default:                       return "E_UNKNOWN"; // 未知错误
    }
}
