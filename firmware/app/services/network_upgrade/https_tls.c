/*
 * 在LwIP netconn上收发TLS记录；未消费的netbuf由会话持有，不另建拼接缓冲。
 */
#include "https_tls.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_monitor.h"
#include "lwip/netbuf.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "ssl_misc.h"

extern void https_tls_register_time(void);

#define TLS_IO_TIMEOUT_MS 10000U

static bool s_module_initialized;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_ctr_drbg;
static uint8_t s_tls_input_buf[MBEDTLS_SSL_IN_BUFFER_LEN]
  __attribute__((section(".bss.ccmram"), aligned(8)));
static uint8_t s_tls_output_buf[MBEDTLS_SSL_OUT_BUFFER_LEN]
  __attribute__((section(".bss.ccmram"), aligned(8)));
static uint8_t s_tls_buffers_used;

// TLS收发区使用固定CCM槽；BIO经CPU复制，不把CCM地址交给网口DMA。
static void *tls_calloc(size_t count, size_t size)
{
  uint8_t slot = 0U;
  void *buffer = NULL;
  if (count == 1U && size == sizeof(s_tls_input_buf))
  {
    slot = 1U;
    buffer = s_tls_input_buf;
  }
  else if (count == 1U && size == sizeof(s_tls_output_buf))
  {
    slot = 2U;
    buffer = s_tls_output_buf;
  }
  else
  {
    return calloc(count, size);
  }
  taskENTER_CRITICAL();
  bool available = (s_tls_buffers_used & slot) == 0U;
  if (available) s_tls_buffers_used |= slot;
  taskEXIT_CRITICAL();
  if (!available) return NULL;
  memset(buffer, 0, size);
  return buffer;
}

// 分别归还CCM收发槽，其他密码对象仍交回原C堆。
static void tls_free(void *pointer)
{
  uint8_t slot;
  if (pointer == s_tls_input_buf)
  {
    slot = 1U;
  }
  else if (pointer == s_tls_output_buf)
  {
    slot = 2U;
  }
  else
  {
    free(pointer);
    return;
  }
  taskENTER_CRITICAL();
  s_tls_buffers_used &= (uint8_t)~slot;
  taskEXIT_CRITICAL();
}

// 按同一开始时刻计算剩余期限，HAL tick回绕不延长会话。
static uint32_t tls_remaining(const https_tls_session_t *session)
{
  uint32_t elapsed = HAL_GetTick() - session->started_ms;
  return elapsed < session->timeout_ms ? session->timeout_ms - elapsed : 0U;
}

// 释放当前TCP缓冲，关闭或读取错误后不能再消费其内容。
static void tls_release_rx(https_tls_session_t *session)
{
  if (session->rx_buf != NULL)
  {
    netbuf_delete(session->rx_buf);
    session->rx_buf = NULL;
  }
  session->rx_offset = 0U;
}

// 将已实际写入TCP的字节数交给mbedTLS，部分写入由其继续发送。
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
  https_tls_session_t *session = (https_tls_session_t *)ctx;
  uint32_t remaining = tls_remaining(session);
  if (remaining == 0U) return MBEDTLS_ERR_SSL_TIMEOUT;
  netconn_set_sendtimeout(session->conn,
                         remaining < TLS_IO_TIMEOUT_MS ? remaining : TLS_IO_TIMEOUT_MS);
  if (!Monitor_UpgradeWait(remaining < TLS_IO_TIMEOUT_MS ? remaining : TLS_IO_TIMEOUT_MS))
    return MBEDTLS_ERR_SSL_TIMEOUT;

  size_t written = 0U;
  err_t result = netconn_write_partly(session->conn, buf, len, NETCONN_COPY, &written);
  if (!Monitor_UpgradeProgress() || tls_remaining(session) == 0U)
    return MBEDTLS_ERR_SSL_TIMEOUT;
  if (written > 0U) return (int)written;
  if (result == ERR_TIMEOUT) return MBEDTLS_ERR_SSL_TIMEOUT;
  if (result == ERR_CLSD || result == ERR_CONN) return MBEDTLS_ERR_NET_CONN_RESET;
  return MBEDTLS_ERR_NET_SEND_FAILED;
}

// 逐段消费netbuf；只有整条链消费完或关闭时才释放，保留未读偏移。
static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
  https_tls_session_t *session = (https_tls_session_t *)ctx;
  if (len == 0U) return 0;
  while (true)
  {
    uint32_t remaining = tls_remaining(session);
    if (remaining == 0U) return MBEDTLS_ERR_SSL_TIMEOUT;
    if (session->rx_buf == NULL)
    {
      netconn_set_recvtimeout(session->conn,
                             remaining < TLS_IO_TIMEOUT_MS ? remaining : TLS_IO_TIMEOUT_MS);
      if (!Monitor_UpgradeWait(remaining < TLS_IO_TIMEOUT_MS ? remaining : TLS_IO_TIMEOUT_MS))
        return MBEDTLS_ERR_SSL_TIMEOUT;
      err_t result = netconn_recv(session->conn, &session->rx_buf);
      if (!Monitor_UpgradeProgress() || tls_remaining(session) == 0U)
      {
        tls_release_rx(session);
        return MBEDTLS_ERR_SSL_TIMEOUT;
      }
      if (result != ERR_OK || session->rx_buf == NULL)
      {
        tls_release_rx(session);
        if (result == ERR_CLSD) return 0;
        if (result == ERR_TIMEOUT) return MBEDTLS_ERR_SSL_TIMEOUT;
        return MBEDTLS_ERR_NET_RECV_FAILED;
      }
      netbuf_first(session->rx_buf);
    }

    void *data = NULL;
    uint16_t size = 0U;
    if (netbuf_data(session->rx_buf, &data, &size) != ERR_OK ||
        session->rx_offset > size || (size != 0U && data == NULL))
    {
      tls_release_rx(session);
      return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    size_t available = size - session->rx_offset;
    size_t copied = available < len ? available : len;
    if (copied > 0U)
      memcpy(buf, (const uint8_t *)data + session->rx_offset, copied);
    session->rx_offset += (uint16_t)copied;
    if (session->rx_offset == size)
    {
      session->rx_offset = 0U;
      // 返回1仍表示成功移到最后一段，负数才表示没有下一段。
      if (netbuf_next(session->rx_buf) < 0) tls_release_rx(session);
    }
    if (copied > 0U) return (int)copied;
  }
}

// 注册UTC时间函数并播种DRBG；失败释放已分配对象，允许之后重新初始化。
https_tls_result_t HttpsTls_Init(void)
{
  if (s_module_initialized) return HTTPS_TLS_OK;
  if (mbedtls_platform_set_calloc_free(tls_calloc, tls_free) != 0)
    return HTTPS_TLS_ERR_INIT;
  https_tls_register_time();
  mbedtls_entropy_init(&s_entropy);
  mbedtls_ctr_drbg_init(&s_ctr_drbg);
  const char *pers = "https_ota_download";
  int result = mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func, &s_entropy,
                                   (const unsigned char *)pers, strlen(pers));
  if (result != 0)
  {
    printf("[TLS] seed fail: -0x%04x\r\n", (unsigned)-result);
    mbedtls_ctr_drbg_free(&s_ctr_drbg);
    mbedtls_entropy_free(&s_entropy);
    return HTTPS_TLS_ERR_INIT;
  }
  s_module_initialized = true;
  return HTTPS_TLS_OK;
}

// 按服务器域名和预置CA执行TLS1.2握手，失败保留TCP所有权供Close收尾。
https_tls_result_t HttpsTls_Connect(https_tls_session_t *session,
                                  struct netconn *conn,
                                  const char *hostname,
                                  const uint8_t *ca_cert_der,
                                  size_t ca_cert_len,
                                  uint32_t timeout_ms)
{
  if (!s_module_initialized) return HTTPS_TLS_ERR_INIT;
  if (session == NULL || conn == NULL || hostname == NULL || hostname[0] == '\0' ||
      ca_cert_der == NULL || ca_cert_len == 0U || timeout_ms == 0U ||
      timeout_ms > INT32_MAX || session->conn != NULL || session->rx_buf != NULL)
    return HTTPS_TLS_ERR_INTERNAL;

  memset(session, 0, sizeof(*session));
  session->conn = conn;
  session->started_ms = HAL_GetTick();
  session->timeout_ms = timeout_ms;
  mbedtls_ssl_init(&session->ssl);
  mbedtls_ssl_config_init(&session->conf);
  mbedtls_x509_crt_init(&session->cacert);
  https_tls_result_t failure = HTTPS_TLS_ERR_HANDSHAKE;

  int result = mbedtls_x509_crt_parse_der_nocopy(&session->cacert, ca_cert_der, ca_cert_len);
  if (result == 0)
    result = mbedtls_ssl_config_defaults(&session->conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (result != 0) goto failed;

  mbedtls_ssl_conf_ca_chain(&session->conf, &session->cacert, NULL);
  mbedtls_ssl_conf_authmode(&session->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_rng(&session->conf, mbedtls_ctr_drbg_random, &s_ctr_drbg);
  result = mbedtls_ssl_conf_max_frag_len(&session->conf, MBEDTLS_SSL_MAX_FRAG_LEN_2048);
  if (result == 0) result = mbedtls_ssl_setup(&session->ssl, &session->conf);
  if (result == 0) result = mbedtls_ssl_set_hostname(&session->ssl, hostname);
  if (result != 0) goto failed;
  mbedtls_ssl_set_bio(&session->ssl, session, bio_send, bio_recv, NULL);

  printf("[TLS] handshake host=%s\r\n", hostname);
  do
  {
    if (tls_remaining(session) == 0U)
    {
      result = MBEDTLS_ERR_SSL_TIMEOUT;
      break;
    }
    result = mbedtls_ssl_handshake(&session->ssl);
  } while (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE);
  if (result == 0 && tls_remaining(session) == 0U) result = MBEDTLS_ERR_SSL_TIMEOUT;
  if (result != 0) goto failed;
  if (mbedtls_ssl_get_verify_result(&session->ssl) != 0U)
  {
    result = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    goto failed;
  }

  session->active = true;
  printf("[TLS] verified: %s\r\n", mbedtls_ssl_get_ciphersuite(&session->ssl));
  return HTTPS_TLS_OK;

failed:
  if (result == MBEDTLS_ERR_SSL_TIMEOUT) failure = HTTPS_TLS_ERR_TIMEOUT;
  printf("[TLS] handshake fail: -0x%04x flags=0x%08lx\r\n",
         (unsigned)-result, (unsigned long)mbedtls_ssl_get_verify_result(&session->ssl));
  tls_release_rx(session);
  mbedtls_ssl_free(&session->ssl);
  mbedtls_ssl_config_free(&session->conf);
  mbedtls_x509_crt_free(&session->cacert);
  return failure;
}

// 完整发送输入，部分写入只推进实际偏移；失败统一返回负数。
int HttpsTls_Send(https_tls_session_t *session, const uint8_t *data, size_t len)
{
  if (session == NULL || !session->active) return -HTTPS_TLS_ERR_INIT;
  if ((data == NULL && len != 0U) || len > INT_MAX) return -HTTPS_TLS_ERR_SEND;
  size_t sent = 0U;
  while (sent < len)
  {
    if (tls_remaining(session) == 0U) return -HTTPS_TLS_ERR_TIMEOUT;
    int result = mbedtls_ssl_write(&session->ssl, data + sent, len - sent);
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (result <= 0)
    {
      printf("[TLS] send fail: -0x%04x\r\n", (unsigned)-result);
      return result == MBEDTLS_ERR_SSL_TIMEOUT ? -HTTPS_TLS_ERR_TIMEOUT : -HTTPS_TLS_ERR_SEND;
    }
    sent += (size_t)result;
  }
  return tls_remaining(session) == 0U ? -HTTPS_TLS_ERR_TIMEOUT : (int)sent;
}

// 读取一段明文；只有TLS结束或TCP EOF返回0，所有错误返回负数。
int HttpsTls_Recv(https_tls_session_t *session, uint8_t *buf, size_t max_len)
{
  if (session == NULL || !session->active) return -HTTPS_TLS_ERR_INIT;
  if (buf == NULL || max_len == 0U || max_len > INT_MAX) return -HTTPS_TLS_ERR_RECV;
  while (true)
  {
    if (tls_remaining(session) == 0U) return -HTTPS_TLS_ERR_TIMEOUT;
    int result = mbedtls_ssl_read(&session->ssl, buf, max_len);
    if (tls_remaining(session) == 0U) return -HTTPS_TLS_ERR_TIMEOUT;
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    if (result < 0)
    {
      printf("[TLS] recv fail: -0x%04x\r\n", (unsigned)-result);
      return result == MBEDTLS_ERR_SSL_TIMEOUT ? -HTTPS_TLS_ERR_TIMEOUT : -HTTPS_TLS_ERR_RECV;
    }
    return result;
  }
}

// 有界尝试发送关闭通知，释放接收链和密码对象；TCP删除失败保留引用。
void HttpsTls_Close(https_tls_session_t *session)
{
  if (session == NULL) return;
  if (session->active)
  {
    int result = mbedtls_ssl_close_notify(&session->ssl);
    if (result != 0)
      printf("[TLS] close notify fail: -0x%04x\r\n", (unsigned)-result);
    session->active = false;
  }
  tls_release_rx(session);
  mbedtls_ssl_free(&session->ssl);
  mbedtls_ssl_config_free(&session->conf);
  mbedtls_x509_crt_free(&session->cacert);
  if (session->conn != NULL)
  {
    uint32_t remaining = tls_remaining(session);
    uint32_t wait_ms = remaining == 0U ? 1U : remaining < TLS_IO_TIMEOUT_MS ? remaining : TLS_IO_TIMEOUT_MS;
    netconn_set_sendtimeout(session->conn, wait_ms);
    if (!Monitor_UpgradeWait(wait_ms)) return;
    err_t result = netconn_delete(session->conn);
    (void)Monitor_UpgradeProgress();
    if (result == ERR_OK)
      session->conn = NULL;
    else
      printf("[TLS] connection delete fail err=%d\r\n", (int)result);
  }
}
