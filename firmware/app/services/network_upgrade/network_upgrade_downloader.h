/*
 * network_upgrade_downloader.h —— HTTP 分块下载器（基于 LwIP netconn）
 *
 * 依据 M4 README：
 *   - "M2将授权后的下载交给升级执行任务，依次调用UpgradeEntry_Begin/Write/Finish"
 *   - Write 每次输入 1～1016 B（UPGRADE_STAGE_MAX_CHUNK_SIZE），offset 为字节偏移
 *   - 30s 无新增读回数据超时，不支持跨会话续传
 *
 * 下载器职责：
 *   1. 解析 URL（host:port/path）
 *   2. DNS 解析 host
 *   3. TCP connect 到 host:port（超时 10s）
 *   4. 发 HTTP GET 请求（带 Range: bytes=0- 或无 Range，取整包）
 *   5. 收响应头，解析 HTTP 状态码 + Content-Length
 *   6. 循环收 body，每收满 ≤1016B 调一次回调
 *   7. 回调返回非 OK 立即中止下载
 *
 * 不做 TLS（百度云 1883 明文接入同理），不做断点续传（M4 不支持跨会话续传）。
 * 整包一次性下载，下载过程中每块交给 UpgradeEntry_Write。
 */
#ifndef NETWORK_UPGRADE_DOWNLOADER_H
#define NETWORK_UPGRADE_DOWNLOADER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 下载结果码 ===== */
typedef enum
{
    NW_DL_OK = 0,              /* 下载完成 */
    NW_DL_ERR_URL,             /* URL 格式错误 */
    NW_DL_ERR_DNS,             /* DNS 解析失败 */
    NW_DL_ERR_CONNECT,         /* TCP 连接失败 */
    NW_DL_ERR_REQUEST,         /* 发送 HTTP 请求失败 */
    NW_DL_ERR_RESPONSE,        /* 接收响应头失败 */
    NW_DL_ERR_HTTP_STATUS,     /* HTTP 状态码非 200/206 */
    NW_DL_ERR_NO_CONTENT_LEN,  /* 响应无 Content-Length */
    NW_DL_ERR_SIZE_MISMATCH,   /* 实际大小 ≠ 期望大小 */
    NW_DL_ERR_RECEIVE,         /* 接收 body 失败 */
    NW_DL_ERR_CALLBACK,        /* 回调返回错误（UpgradeEntry_Write 失败） */
    NW_DL_ERR_TIMEOUT,         /* 超时 */
    NW_DL_ERR_INTERNAL,        /* 内部错误 */
} nw_dl_status_t;

/* ===== 每块回调 =====
 * 下载器每收满 chunk_size 字节调用一次。
 * offset 为本次数据在整包中的字节偏移（从 0 开始递增）。
 * data 指向下载器内部缓冲，回调返回后下载器继续复用，回调内如需保留须拷贝。
 * 返回 NW_DL_OK 继续；返回 NW_DL_ERR_CALLBACK 立即中止下载。
 */
typedef nw_dl_status_t (*nw_dl_chunk_cb)(void *ctx,
                                         uint32_t offset,
                                         const uint8_t *data,
                                         uint32_t len);

/* ===== 下载参数 ===== */
typedef struct
{
    const char *url;          /* http://host[:port]/path，不支持 https */
    const char *auth_token;   /* 可选 Authorization 头值，NULL 表示不加 */
    uint32_t    total_size;   /* 期望总字节数（用于校验 Content-Length） */
    nw_dl_chunk_cb cb;        /* 每块回调（传给 UpgradeEntry_Write） */
    void       *ctx;          /* 回调上下文（UpgradeEntryPeer + session_id 等） */
    uint32_t    timeout_ms;   /* 总超时（默认 120000 = 120s） */
    uint32_t    chunk_size;   /* 每块大小（默认 1016 = UPGRADE_STAGE_MAX_CHUNK_SIZE） */
} nw_dl_params_t;

/**
 * @brief  执行一次 HTTP 下载（整包，不分段 Range）
 * @param  params  下载参数
 * @return NW_DL_OK=成功，其他=失败（错误码见枚举）
 * @note   阻塞调用，须在升级执行任务里调（不在 MQTT 回调里调）。
 *         内部用 netconn，每次收 ≤chunk_size 字节调回调。
 *         仅允许同一升级任务串行调用；释放连接失败时保留引用，下一次先重试释放。
 */
nw_dl_status_t NetworkDownloader_Fetch(const nw_dl_params_t *params);

/**
 * @brief  下载结果码转字符串（供日志和状态上报）
 */
const char *NetworkDownloader_ErrorString(nw_dl_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_UPGRADE_DOWNLOADER_H */
