/*
 * mqtt_client.c —— 最小 MQTT 3.1.1 客户端（骨架）
 *
 * 只做四件事：
 *   1. 等 LwIP/DHCP 就绪；
 *   2. netconn_connect() 到 broker:1883；
 *   3. 发 MQTT CONNECT，等 CONNACK，串口打印返回码；
 *   4. 向 "<gateway_id>/status" publish 一条 "online"。
 *
 * 设计取舍（RAM 紧张，128KB 已用 86%）：
 *   - 发送/接收缓冲全部 static，不占任务栈；
 *   - 不实现订阅、QoS1/2、遗嘱、TLS，留到通了以后再加；
 *   - 出错直接断连，3 秒后重连（最简单的死循环）。
 *
 * 协议参考：OASIS MQTT 3.1.1 协议名 "MQTT"、level=4。
 */
#include "mqtt_client.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "lwip/opt.h"
#include "lwip/api.h"       /* netconn_* */
#include "lwip/ip_addr.h"

/* ===== 配置（先硬编码，后面挪到 app_config_t） ===== */
/* broker 先填 PC 的 IP（之前 ping 回包显示 PC=192.168.2.45） */
#define MQTT_BROKER_IP0  192
#define MQTT_BROKER_IP1  168
#define MQTT_BROKER_IP2  2
#define MQTT_BROKER_IP3  45
#define MQTT_BROKER_PORT 1883

/* gateway_id：和 tests/mqtt 下的样例 json 保持一致 */
#define MQTT_CLIENT_ID   "mb2mqtt_001"
/* status topic = "<gateway_id>/status" */
#define MQTT_STATUS_TOPIC "mb2mqtt_001/status"
/* 下行命令订阅 topic */
#define MQTT_CMD_TOPIC    "mb2mqtt_001/cmd"

/* 收发缓冲：放静态区，不占 4KB 任务栈。
 * tx_buf 要装 topic(≈20B) + payload(≤512B) + MQTT 头，给 640B
 * 挪到 CCMRAM（纯 CPU 暂存，netconn_write 会 memcpy 走，本身不被 DMA 碰） */
static uint8_t tx_buf[640] __attribute__((section(".ccmram")));
static uint8_t rx_buf[64]  __attribute__((section(".ccmram")));

/* 连接状态 + 发送互斥锁：telemetry/replay 任务可并发调 MqttClient_Publish */
static struct netconn *g_conn;          /* NULL=未连接 */
static osMutexId_t     g_tx_mutex;
static const osMutexAttr_t g_tx_mutex_attr = { .name = "mqtt_tx" };

/* ---- MQTT 编码小工具： remaining length 用单字节（我们报文都 <127） ---- */
static size_t mqtt_encode_header(uint8_t type_flags, const uint8_t *variable_and_payload, size_t vplen)
{
  size_t off = 0;
  tx_buf[off++] = (uint8_t)type_flags;
  tx_buf[off++] = (uint8_t)vplen;   /* 我们所有报文都 <127，单字节够了 */
  memcpy(&tx_buf[off], variable_and_payload, vplen);
  return off + vplen;
}

/* 组 CONNECT 报文：协议名 MQTT / level 4 / clean session / keepalive 60s */
static size_t mqtt_build_connect(void)
{
  uint8_t vh[32];
  size_t n = 0;

  /* 可变报头 */
  vh[n++] = 0x00; vh[n++] = 0x04;        /* 协议名长度 = 4 */
  vh[n++] = 'M';  vh[n++] = 'Q';
  vh[n++] = 'T';  vh[n++] = 'T';
  vh[n++] = 0x04;                        /* 协议级别 = MQTT 3.1.1 */
  vh[n++] = 0x02;                        /* CONNECT flags: clean session=1，无遗嘱无用户名密码 */
  vh[n++] = 0x00; vh[n++] = 30;          /* keepalive = 30s */

  /* 载荷：clientID（2 字节长度前缀 + UTF-8） */
  size_t cidlen = strlen(MQTT_CLIENT_ID);
  vh[n++] = (uint8_t)(cidlen >> 8);
  vh[n++] = (uint8_t)(cidlen & 0xFF);
  memcpy(&vh[n], MQTT_CLIENT_ID, cidlen);
  n += cidlen;

  /* 固定头类型 = 0x10（CONNECT） */
  return mqtt_encode_header(0x10, vh, n);
}

/* 组 PUBLISH（QoS0，无 packet id）：topic + payload */
static size_t mqtt_build_publish(const char *topic, const uint8_t *payload, size_t plen)
{
  uint8_t vh[128];
  size_t n = 0;

  size_t tlen = strlen(topic);
  vh[n++] = (uint8_t)(tlen >> 8);
  vh[n++] = (uint8_t)(tlen & 0xFF);
  memcpy(&vh[n], topic, tlen);
  n += tlen;

  memcpy(&vh[n], payload, plen);
  n += plen;

  /* 0x30 = PUBLISH, DUP=0, QoS=0, RETAIN=0 */
  return mqtt_encode_header(0x30, vh, n);
}

/* 组 SUBSCRIBE（packet id=0x0001，订阅一个 topic，QoS0） */
static size_t mqtt_build_subscribe(const char *topic)
{
  uint8_t vh[128];
  size_t n = 0;

  vh[n++] = 0x00; vh[n++] = 0x01;          /* packet id = 1 */
  size_t tlen = strlen(topic);
  vh[n++] = (uint8_t)(tlen >> 8);
  vh[n++] = (uint8_t)(tlen & 0xFF);
  memcpy(&vh[n], topic, tlen);
  n += tlen;
  vh[n++] = 0x00;                          /* 期望 QoS = 0 */

  /* 0x82 = SUBSCRIBE, DUP=0, QoS=1（协议要求 SUBSCRIBE 固定带 QoS1） */
  return mqtt_encode_header(0x82, vh, n);
}

/* 读一个 netbuf 到 rx_buf，返回读到的字节数 */
static uint16_t mqtt_recv_to_buf(struct netconn *conn)
{
  struct netbuf *nb = NULL;
  err_t err = netconn_recv(conn, &nb);
  if (err != ERR_OK || nb == NULL)
  {
    if (nb) netbuf_delete(nb);
    return 0;
  }
  uint16_t rlen = 0;
  struct pbuf *chain = nb->p;
  while (chain && rlen < sizeof(rx_buf))
  {
    uint16_t copy = chain->len;
    if (rlen + copy > sizeof(rx_buf)) copy = sizeof(rx_buf) - rlen;
    memcpy(&rx_buf[rlen], chain->payload, copy);
    rlen += copy;
    chain = chain->next;
  }
  netbuf_delete(nb);
  return rlen;
}

/* 线程安全的 PUBLISH（QoS0）：telemetry/replay 任务调。
 * 直接把报文拼进 tx_buf，锁保护下 netconn_write。未连接返回 -1。 */
int MqttClient_Publish(const char *topic, const uint8_t *payload, size_t plen)
{
    if (topic == NULL || payload == NULL || plen == 0) return -1;
    if (g_conn == NULL) return -1;

    osMutexAcquire(g_tx_mutex, osWaitForever);

    size_t tlen = strlen(topic);
    size_t vplen = 2 + tlen + plen;   /* 可变头：topic_len(2) + topic + payload */
    /* remaining length 最多 4 字节编码 */
    uint8_t rlbuf[4];
    size_t rllen = 0;
    size_t enc = vplen;
    do {
        uint8_t b = enc & 0x7F;
        enc >>= 7;
        if (enc > 0) b |= 0x80;
        rlbuf[rllen++] = b;
    } while (enc > 0 && rllen < 4);

    /* 总长度 = fixed_header(1) + remaining_len(rllen) + vplen */
    if (1 + rllen + vplen > sizeof(tx_buf)) {
        osMutexRelease(g_tx_mutex);
        return -2;
    }

    size_t off = 0;
    tx_buf[off++] = 0x30;               /* PUBLISH QoS0 */
    memcpy(&tx_buf[off], rlbuf, rllen); off += rllen;
    tx_buf[off++] = (uint8_t)(tlen >> 8);
    tx_buf[off++] = (uint8_t)(tlen & 0xFF);
    memcpy(&tx_buf[off], topic, tlen); off += tlen;
    memcpy(&tx_buf[off], payload, plen); off += plen;

    err_t err = netconn_write(g_conn, tx_buf, off, NETCONN_COPY);

    osMutexRelease(g_tx_mutex);
    return (err == ERR_OK) ? 0 : -3;
}

AppModuleInitResult MqttClient_Init(void)
{
  /* 创建发送互斥锁 */
  g_tx_mutex = osMutexNew(&g_tx_mutex_attr);
  return APP_MODULE_INIT_OK;
}

void MqttClient_Task(void *argument)
{
  (void)argument;

  ip_addr_t broker_ip;
  IP4_ADDR(&broker_ip, MQTT_BROKER_IP0, MQTT_BROKER_IP1, MQTT_BROKER_IP2, MQTT_BROKER_IP3);

  /* 先等 5 秒让 DHCP 拿到 IP（g_m2_eth_inited=1 后网络才通） */
  osDelay(pdMS_TO_TICKS(5000));

  for (;;)
  {
    printf("[MQTT] connecting to broker %u.%u.%u.%u:%u ...\r\n",
           MQTT_BROKER_IP0, MQTT_BROKER_IP1, MQTT_BROKER_IP2, MQTT_BROKER_IP3,
           (unsigned)MQTT_BROKER_PORT);

    /* 1) 建 TCP 连接 */
    struct netconn *conn = netconn_new(NETCONN_TCP);
    if (conn == NULL)
    {
      printf("[MQTT] netconn_new failed\r\n");
      osDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    netconn_set_recvtimeout(conn, 5000);   /* 5s 收超时，避免任务永久阻塞 */

    err_t err = netconn_connect(conn, &broker_ip, MQTT_BROKER_PORT);
    if (err != ERR_OK)
    {
      printf("[MQTT] connect failed err=%d\r\n", (int)err);
      netconn_delete(conn);
      osDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    printf("[MQTT] TCP connected, sending CONNECT\r\n");

    /* 2) 发 CONNECT */
    size_t pkglen = mqtt_build_connect();
    err = netconn_write(conn, tx_buf, pkglen, NETCONN_COPY);
    if (err != ERR_OK)
    {
      printf("[MQTT] write CONNECT failed err=%d\r\n", (int)err);
      netconn_delete(conn);
      osDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    /* 3) 等 CONNACK：固定 4 字节（0x20 0x02 0x00 0x00=接受） */
    struct netbuf *nb = NULL;
    err = netconn_recv(conn, &nb);
    if (err != ERR_OK || nb == NULL)
    {
      printf("[MQTT] recv CONNACK failed err=%d\r\n", (int)err);
      if (nb) netbuf_delete(nb);
      netconn_delete(conn);
      osDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    /* 把 netbuf 拷进 rx_buf（小，一次拷完） */
    uint16_t rlen = 0;
    struct pbuf *chain = nb->p;
    while (chain && rlen < sizeof(rx_buf))
    {
      memcpy(&rx_buf[rlen], chain->payload, chain->len);
      rlen += chain->len;
      chain = chain->next;
    }
    netbuf_delete(nb);

    if (rlen < 4 || rx_buf[0] != 0x20)
    {
      printf("[MQTT] bad CONNACK: %02X %02X %02X %02X (len=%u)\r\n",
             rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], (unsigned)rlen);
      netconn_delete(conn);
      osDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    printf("[MQTT] CONNACK return code = %u (0=Accepted)\r\n", (unsigned)rx_buf[3]);
    g_conn = conn;   /* 标记连接已建立，允许其他任务 publish */

    /* 4) PUBLISH status=online */
    pkglen = mqtt_build_publish(MQTT_STATUS_TOPIC, (const uint8_t *)"online", 6);
    err = netconn_write(conn, tx_buf, pkglen, NETCONN_COPY);
    printf("[MQTT] PUBLISH status 'online' -> %s, ret=%d\r\n", MQTT_STATUS_TOPIC, (int)err);

    /* 5) 订阅下行命令 topic */
    pkglen = mqtt_build_subscribe(MQTT_CMD_TOPIC);
    err = netconn_write(conn, tx_buf, pkglen, NETCONN_COPY);
    printf("[MQTT] SUBSCRIBE %s, ret=%d\r\n", MQTT_CMD_TOPIC, (int)err);
    (void)mqtt_recv_to_buf(conn);
    printf("[MQTT] SUBACK: %02X %02X %02X %02X %02X\r\n",
           rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4]);

    /* 6) 常驻 recv 循环：收 broker 下行，5s 收不到就累计，满 5 次发 PINGREQ 心跳 */
    unsigned idle_ticks = 0;
    bool broken = false;
    while (!broken)
    {
      uint16_t n = mqtt_recv_to_buf(conn);
      if (n == 0)
      {
        /* 收超时：累计，每 25s 发一次 PINGREQ（keepalive=30s） */
        idle_ticks++;
        if (idle_ticks >= 5)
        {
          tx_buf[0] = 0xC0; tx_buf[1] = 0x00;   /* PINGREQ */
          err = netconn_write(conn, tx_buf, 2, NETCONN_COPY);
          printf("[MQTT] PINGREQ sent, ret=%d\r\n", (int)err);
          idle_ticks = 0;
        }
        continue;
      }
      idle_ticks = 0;

      /* 按 MQTT 报文类型分流（骨架阶段只打日志，不解析 payload） */
      uint8_t ptype = rx_buf[0] & 0xF0;
      switch (ptype)
      {
        case 0xD0:  printf("[MQTT] recv PINGRESP\r\n"); break;
        case 0x30:  /* PUBLISH：broker 下行命令 */
        case 0x31:
          printf("[MQTT] recv PUBLISH from broker, %u bytes\r\n", (unsigned)n);
          /* 后续：解析 topic/payload，调 M1_ExecuteCommand */
          break;
        default:
          printf("[MQTT] recv unknown type=%02X len=%u\r\n", rx_buf[0], (unsigned)n);
          break;
      }
    }

    /* 走到这里说明连接异常，外层 3 秒后重连 */
    g_conn = NULL;
    netconn_close(conn);
    netconn_delete(conn);
    osDelay(pdMS_TO_TICKS(3000));
  }
}
