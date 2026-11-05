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

/* ★协议栈堆内存大小：16KB
   lwip 的动态分配（mem_malloc）都从这块堆走，TCP 发送缓冲等大块内存也在这。
   例程原值 16KB（F407 够跑 TCP/UDP/DHCP），不够时看编译期 LWIP_MEM 统计再加 */
#define MEM_SIZE                    (16*1024)

/* 内存池数量（MEMP_NUM_xxx = 每种对象预分配的个数，编译期固定）：
   例程原值，对应 F407 上"同时 4 个 TCP + 2 个监听 + 20 个收包缓冲"的典型用量 */
#define MEMP_NUM_PBUF               15
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define MEMP_NUM_TCP_SEG            150   /* TCP 分段缓冲（150 段，配合 TCP_WND 用） */
#define MEMP_NUM_SYS_TIMEOUT        6     /* 系统超时回调槽位（tcp/arp/dhcp/igmp 等要注册超时） */

/* ---------- Pbuf 选项 ---------- */
#define PBUF_POOL_SIZE              20    /* pbuf 池个数（收包缓冲，配下面单块大小） */
/* ★单块 pbuf 大小 = TCP_MSS(1460) + 40(IP+TCP头) + 链路层头尾 ≈ 1514，
   正好装得下一个完整以太网帧。LWIP_MEM_ALIGN_SIZE 按 4 对齐 */
#define PBUF_POOL_BUFSIZE           LWIP_MEM_ALIGN_SIZE(TCP_MSS+40+PBUF_LINK_ENCAPSULATION_HLEN+PBUF_LINK_HLEN)

/* ---------- TCP 选项 ---------- */
#define LWIP_TCP                    1     /* 开 TCP（以后 MQTT 走 TCP） */
#define TCP_TTL                     255   /* TCP 报文 TTL=255（最大，本地网无所谓） */
#define TCP_QUEUE_OOSEQ             0     /* ★不缓存乱序到达的 TCP 段
   省内存：F407 内存有限，乱序缓存很吃内存，0 = 乱序段直接丢弃由 TCP 重传机制兜底 */
#define TCP_MSS                     (1500 - 40)  /* ★最大段大小 = MTU 1500 - IP头20 - TCP头20 = 1460 */
#define TCP_SND_BUF                 (11*TCP_MSS) /* 发送缓冲 11 段 ≈ 16KB（一个完整 MSS 窗口） */
#define TCP_SND_QUEUELEN            (8*TCP_SND_BUF/TCP_MSS)  /* 发送队列长度（可排队 8 个缓冲窗口） */
#define TCP_WND                     (20*TCP_MSS) /* 接收窗口 20 段 ≈ 29KB（对方能连发这么多不用等 ACK） */

/* ---------- ICMP（★ping 必须，不开 ping 不通） ---------- */
#define LWIP_ICMP                   1

/* ---------- DHCP ---------- */
#define LWIP_DHCP                   1     /* ★开 DHCP：自动从路由器要 IP，超时回落静态（ethernet.c） */

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

/* ---------- API：带 OS 用 Netconn + Socket ---------- */
/* ★LWIP_NETCONN=1：提供 netconn API（lwip 的高级线程安全 API，例程 TCP 客户端用它）
   LWIP_SOCKET=1：提供类 BSD socket API（以后 MQTT 客户端库一般用 socket）
   两个都开：以后 Day3 MQTT 移植时不挑库 */
#define LWIP_NETCONN                1
#define LWIP_SOCKET                 1

/* ---------- 操作系统选项（线程配置，例程原值） ---------- */
/* 默认邮箱大小（netconn/socket 层的收包邮箱容量，10 条消息缓存） */
#define DEFAULT_UDP_RECVMBOX_SIZE       10
#define DEFAULT_TCP_RECVMBOX_SIZE       10
#define DEFAULT_ACCEPTMBOX_SIZE         10
/* 默认线程栈：1KB（sys_arch 会把"字节"换算成 FreeRTOS 的"字"） */
#define DEFAULT_THREAD_STACKSIZE        1024

/* ★tcpip 内核线程配置：
   名字 "lwip_thread"、栈 2048 字节（协议栈处理活重，栈要给足）、
   邮箱 8 条、优先级 = configMAX_PRIORITIES-2（最高档，但留 2 级给更关键的中断级任务）。
   所有协议栈处理都在这线程里，优先级必须高于业务线程 */
#define TCPIP_THREAD_NAME              "lwip_thread"
#define TCPIP_THREAD_STACKSIZE          2048
#define TCPIP_MBOX_SIZE                 8
#define TCPIP_THREAD_PRIO               (configMAX_PRIORITIES - 2)
#define LWIP_SO_RCVTIMEO                1   /* 允许 socket 接收设超时（以后 MQTT 库轮询用得到） */

/* ---------- 调试 ---------- */
#define LWIP_DEBUG                      LWIP_DBG_OFF   /* ★关闭 lwip 内部调试打印（否则串口刷屏） */
#define ICMP_DEBUG                      LWIP_DBG_OFF   /* 关 ICMP 调试（ping 通了就行，不刷屏） */

#endif /* __LWIPOPTS_H__ */
