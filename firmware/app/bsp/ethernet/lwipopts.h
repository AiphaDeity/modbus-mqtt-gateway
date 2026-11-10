/**
 * @file    lwipopts.h
 * @brief   LwIP 配置（Day2 带 OS 版 · 抄正点原子探索者例程原版）
 *
 * 【这份文件放哪】
 *   bsp/ethernet/ 目录下（跟 ethernetif.c 一起）。
 *   说明：lwip 的 opt.h 在末尾会 include "lwipopts.h"（按 include 搜索路径找），
 *   bsp/ethernet 目录已通过 m2_ethernet 库 PUBLIC 暴露，lwip 库也能搜到。
 *
 * 【这个文件是干什么的】
 *   lwip 协议栈有几百个可配置项，opt.h 里全是默认值。
 *   这个文件"覆盖默认值"：协议栈编译时最后读这里的配置。
 *   改错 = 行为怪异或编译错；所以全部数值照例程原版，一个别动。
 *
 * 【为什么是带 OS 版】
 *   同事已把 FreeRTOS 接进工程（main.c 里 osKernelStart() 接管主循环），
 *   裸跑轮询没地方放，所以 LwIP 跑在 FreeRTOS 上（NO_SYS=0）。
 *   这份配置 = 探索者例程 arch/lwipopts.h 原版，只删掉了注释、保留了全部数值。
 *
 * 【关键点】
 *   - NO_SYS=0 + LWIP_NETCONN/SOCKET=1：用例程现成的 netconf.c / sys_arch.c 体系
 *   - include "FreeRTOSConfig.h"：例程依赖（TCPIP_THREAD_PRIO 用它的宏），
 *     同事工程在 Core/Inc 已有此文件
 */
#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

/* ★为什么 include FreeRTOSConfig.h？
   下面 TCPIP_THREAD_PRIO 用了 configMAX_PRIORITIES 宏，这个宏定义在
   FreeRTOSConfig.h（同事工程 Core/Inc 下）。协议栈编译时要能搜到这个头，
   所以 lwip 库在 CMake 里必须链接 stm32cubemx（它传递 Core/Inc 的 include 路径） */
#include "FreeRTOSConfig.h"

/* 任务间保护（SYS_LIGHTWEIGHT_PROT=1）
   ★带 OS 时必须开：多个线程（业务线程 + tcpip 内核线程）会并发访问协议栈
   内部数据结构（链表等），lwip 用它内部的轻量保护防止数据竞争 */
#define SYS_LIGHTWEIGHT_PROT        1

/* NO_SYS=0：有操作系统（FreeRTOS）
   ★这个宏决定了整个编译体系：
     =1 裸跑：没有线程概念，协议栈在主循环里轮询，sys.c 用空实现
     =0 带 OS：lwip 的 tcpip_thread 等线程由 sys_arch.c 创建（FreeRTOS 任务），
        API 调用会走线程间通信（邮箱/信号量） */
#define NO_SYS                      0
#define NO_SYS_NO_TIMERS            0

/* ---------- 内存选项（沿用探索者例程） ---------- */
/* 4 字节对齐：M4 内核要求指针/结构体按 4 字节边界对齐，
   否则 DMA 和协议栈读写可能出问题 */
#define MEM_ALIGNMENT               4

/* ★协议栈堆内存大小：12KB
   lwip 的动态分配（mem_malloc）都从这块堆走，TCP 发送缓冲等大块内存也在这。
   与 FreeRTOS heap(32KB) 独立。MQTT 客户端 TCP 发送缓冲走 FreeRTOS heap，
   lwip 这边够 DHCP/UDP/TCPIP mailbox 就行。SRAM 128KB 装得下 MEM_SIZE 12KB + heap 32KB。 */
#define MEM_SIZE                    (12*1024)

/* 内存池数量（Day3 裁剪：单 MQTT 连接够用） */
#define MEMP_NUM_PBUF               6     /* 原 15：原始 pbuf，只给 IP 分片用，6 够 */
#define MEMP_NUM_UDP_PCB            2     /* 原 4：DHCP 占 1 个，留 1 个余量 */
#define MEMP_NUM_TCP_PCB            2     /* 原 4：MQTT 只占 1 个，留 1 个余量 */
#define MEMP_NUM_TCP_PCB_LISTEN     0     /* 原 2：我们不做 TCP 服务器，关掉 */
#define MEMP_NUM_TCP_SEG            32    /* 原 150：TCP 分段缓冲，32 段够发小 JSON */
#define MEMP_NUM_SYS_TIMEOUT        8     /* 原 6：arp/dhcp/tcp；开 DNS 后需 +1 超时槽位，给 8 */

/* ---------- Pbuf 选项 ---------- */
/* ★★2026-11-10：原 8 改为 12。
   虽然零拷贝 RX 用的是自定义 RX_POOL（不走 lwip PBUF_POOL），
   但 lwip 内部 IP 分片/ARP 队列/TCP 重传等仍需从 PBUF_POOL 分配。
   TCP 连云期间 ARP+DHCP+DNS+TCP 并发，8 个 pbuf 偏紧，
   +4 个余量避免 lwip 内部分配失败导致丢帧。RAM 代价 ~6KB */
#define PBUF_POOL_SIZE              12
/* ★单块 pbuf 大小 = TCP_MSS(1460) + 40(IP+TCP头) + 链路层头尾 ≈ 1514，
   正好装得下一个完整以太网帧。LWIP_MEM_ALIGN_SIZE 按 4 对齐 */
#define PBUF_POOL_BUFSIZE           LWIP_MEM_ALIGN_SIZE(TCP_MSS+40+PBUF_LINK_ENCAPSULATION_HLEN+PBUF_LINK_HLEN)

/* ---------- TCP 选项 ---------- */
#define LWIP_TCP                    1     /* 开 TCP（以后 MQTT 走 TCP） */
#define TCP_TTL                     255   /* TCP 报文 TTL=255（最大，本地网无所谓） */
#define TCP_QUEUE_OOSEQ             0     /* 不缓存乱序段，省内存 */
#define TCP_MSS                     (1500 - 40)  /* 最大段大小 = 1460 */
/* 原 11×MSS=16KB 发送缓冲，我们只发 200B JSON，2×MSS=2.9KB 够 */
#define TCP_SND_BUF                 (2*TCP_MSS)
#define TCP_SND_QUEUELEN            (4*TCP_SND_BUF/TCP_MSS)
/* 原 20×MSS=29KB 接收窗口，我们只收 broker 的 PINGRESP/命令包，4×MSS=5.8KB 够 */
#define TCP_WND                     (4*TCP_MSS)

/* ---------- ICMP（★ping 必须，不开 ping 不通） ---------- */
#define LWIP_ICMP                   1

/* ---------- DHCP ---------- */
#define LWIP_DHCP                   1     /* ★开 DHCP：自动从路由器要 IP，超时回落静态（ethernet.c） */

/* ---------- DNS（★连云必须：云平台 broker 都是域名，不开 DNS 解析不了） ---------- */
#define LWIP_DNS                   1     /* ★开 DNS：解析阿里云 broker 域名 */
/* DHCP 开启时 LwIP 自动从 DHCP 服务器拿 DNS（LWIP_DHCP_GET_SERVER_DNS 默认随 LWIP_DHCP 开）；
 * 静态 IP 兜底时由 ethernet.c 手动设 114.114.114.114。LWIP_DNS_MAX_SERVERS 默认 2 够用。 */

/* LWIP_RAND：DNS 生成 txid/source port 用。例程未定义，这里给固定值（调试够用，
 *   不防预测；量产应换成真随机源如 STM32 RNG）。固定 txid 不影响 DNS 解析正确性。 */
#define LWIP_RAND()  ((u32_t)0xC0FFEE01u)

/* ---------- UDP ---------- */
#define LWIP_UDP                    1     /* 开 UDP（DHCP/SNTP 等都要 UDP） */
#define UDP_TTL                     255

/* ---------- Statistics / errno ---------- */
#define LWIP_STATS                  0     /* 关统计（省内存省代码） */
#define LWIP_PROVIDE_ERRNO          1     /* 提供 errno（socket API 需要，否则 lwip 不提供） */

/* ---------- 链接回调 ---------- */
#define LWIP_NETIF_LINK_CALLBACK    1     /* ★网线插拔回调：ethernet.c 的 lwip_link_status_updated 靠它触发 */

/* ---------- 校验和：F407 ETH 硬件计算 ---------- */
/* ★F407 的 ETH 硬件能算 IP/UDP/TCP/ICMP 校验和，所以协议栈全部关掉软件计算
   （CHECKSUM_xxx=0），CPU 零负担，收发更快。
   硬件侧由同事 eth.c 的 TxConfig 配好（Attributes=CSUM，ChecksumCtrl 等） */
#define CHECKSUM_BY_HARDWARE
#ifdef CHECKSUM_BY_HARDWARE
  #define CHECKSUM_GEN_IP           0     /* 发 IP 包：硬件生成校验和 */
  #define CHECKSUM_GEN_UDP          0     /* 发 UDP：硬件生成 */
  #define CHECKSUM_GEN_TCP          0     /* 发 TCP：硬件生成 */
  #define CHECKSUM_GEN_ICMP         0     /* 发 ICMP：硬件生成 */
  #define CHECKSUM_CHECK_IP         0     /* 收 IP：硬件校验 */
  #define CHECKSUM_CHECK_UDP        0     /* 收 UDP：硬件校验 */
  #define CHECKSUM_CHECK_TCP        0     /* 收 TCP：硬件校验 */
#else
  /* （软件算校验和时才用，我们不开，保留备查） */
  #define CHECKSUM_GEN_IP           1
  #define CHECKSUM_GEN_UDP          1
  #define CHECKSUM_GEN_TCP          1
  #define CHECKSUM_GEN_ICMP         1
  #define CHECKSUM_CHECK_IP         1
  #define CHECKSUM_CHECK_UDP        1
  #define CHECKSUM_CHECK_TCP        1
#endif

/* ---------- API：带 OS 用 Netconn（不用 socket） ---------- */
/* LWIP_NETCONN=1：netconn API，我们 MQTT 客户端直接用它 */
#define LWIP_NETCONN                1
/* LWIP_SOCKET=0：不用 BSD socket API，省代码省内存（我们 netconn 手写 MQTT） */
#define LWIP_SOCKET                 0
/* LWIP_NETIF_API=1：netifapi_* 线程安全封装，链路/DHCP 要用 */
#define LWIP_NETIF_API              1

/* ---------- 操作系统选项（Day3 裁剪） ---------- */
/* 邮箱大小：原 10，单连接 4 够 */
#define DEFAULT_UDP_RECVMBOX_SIZE       4
#define DEFAULT_TCP_RECVMBOX_SIZE       4
#define DEFAULT_ACCEPTMBOX_SIZE         4
/* 默认线程栈：1KB */
#define DEFAULT_THREAD_STACKSIZE        1024

/* tcpip 内核线程 */
#define TCPIP_THREAD_NAME              "lwip_thread"
#define TCPIP_THREAD_STACKSIZE          1280
#define TCPIP_MBOX_SIZE                 4     /* 原 8，裁剪一半 */
#define TCPIP_THREAD_PRIO               (configMAX_PRIORITIES - 2)
#define LWIP_SO_RCVTIMEO                1   /* netconn 收超时要用 */

/* ---------- 调试 ---------- */
#define LWIP_DEBUG                      LWIP_DBG_OFF   /* ★关闭 lwip 内部调试打印（否则串口刷屏） */
#define ICMP_DEBUG                      LWIP_DBG_OFF   /* 关 ICMP 调试（ping 通了就行，不刷屏） */

#endif /* __LWIPOPTS_H__ */
