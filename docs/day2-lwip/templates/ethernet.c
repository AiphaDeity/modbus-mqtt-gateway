/**
 * @file    ethernet.c
 * @brief   以太网业务层（Day2 带 OS 版 · LwIP 接入 + DHCP/链接线程）
 *
 * 【这份文件放哪】bsp/ethernet/ 目录下
 *
 * 【这个文件是干什么的】
 *   工程里现在有：FreeRTOS（同事）、ETH 硬件驱动（同事 eth.c）、
 *   lwip 协议栈（已拷）、网卡驱动层（第 2 步 ethernetif.c）。
 *   本文件是把它们"串起来"的那一层——负责：
 *     1) 启动 lwip 内核（tcpip_thread）
 *     2) 把网卡挂进 lwip（netif_add）
 *     3) 派两个线程：DHCP 线程（自动要 IP）、链接线程（盯网线）
 *
 * 【逻辑出处】抄正点原子探索者例程 arch/netconf.c（LwIP_Init / lwip_dhcp_process /
 *            lwip_link_thread），把标准库 ETH_ReadPHYRegister/ETH_Stop/ETH_Start
 *            换成 HAL 的 lan8720_link_read()/HAL_ETH_Stop/HAL_ETH_Start_IT
 */

/* ================= 一、头文件：每个都是干什么的 ================= */
#include "lwip/mem.h"        /* lwip 堆内存 API（mem_malloc 等，本文件没直接用，netif 体系需要） */
#include "lwip/memp.h"       /* lwip 内存池 API（memp_xxx，netif 内部依赖） */
#include "lwip/tcp.h"        /* TCP 协议控制块类型（netif 关联 TCP 需要） */
#include "lwip/tcpip.h"      /* ★核心：tcpip_init()（创建 lwip 内核线程）+ tcpip_input()（喂包入口） */
#include "lwip/udp.h"        /* UDP 控制块类型 */
#include "netif/etharp.h"    /* ARP 模块（etharp_output 在 ethernetif 里用） */
#include "lwip/dhcp.h"       /* ★DHCP：dhcp_start/dhcp_stop/dhcp_supplied_address */
#include "lwip/init.h"       /* lwip_init（tcpip_init 内部会调，不用直接调） */
#include "lwip/timeouts.h"   /* lwip 超时管理（tcpip 线程内部用） */
#include "lwip/netif.h"      /* ★netif 结构体：netif_add/netif_set_default/netif_is_link_up 等 */

#include "FreeRTOS.h"        /* ★FreeRTOS 主头：taskENTER_CRITICAL 等 */
#include "task.h"            /* ★vTaskDelay（线程延时） */

#include "stm32f4xx_hal.h"   /* HAL 主头：HAL_ETH_Stop / HAL_ETH_Start_IT */
#include <stdio.h>           /* printf（同事已在 main.c 实现重定向，直接能用） */

#include "ethernet.h"        /* 本模块对外接口（Day1 约定） */
#include "ethernetif.h"      /* 网卡驱动：ethernetif_init（netif_add 的回调）+ lan8720_link_read */

/* 同事 Core/Src/eth.c 里的 ETH 句柄。
   链接线程里要 HAL_ETH_Stop/Start_IT 操作这个句柄，所以必须 extern 声明。
   （不是重复定义！定义在同事 eth.c，这里只是"借用"） */
extern ETH_HandleTypeDef heth;

/* ================= 二、全局控制结构（照探索者例程） ================= */
/* lwip 控制结构体：记录本机 IP/掩码/网关/状态，供调试打印和 DHCP 回落用 */
typedef struct
{
  uint8_t  remoteip[4];   /* 远端 IP（探索者例程 TCP 客户端测试用，M2 暂时用不到，保留字段） */
  uint8_t  ip[4];         /* 本机 IP：DHCP 成功时更新 / DHCP 超时回落用静态值 */
  uint8_t  netmask[4];    /* 子网掩码，默认 255.255.255.0 */
  uint8_t  gateway[4];    /* 默认网关，默认 192.168.1.1 */
  uint8_t  dhcpstatus;    /* DHCP 状态标记：0=没用DHCP 1=请求中 2=成功 0xFF=超时已回落静态 */
  uint8_t  link_status;   /* 链路状态：0=断链 1=已链接（链接线程更新，打印/业务用） */
} __lwip_dev;

static __lwip_dev g_lwipdev;          /* lwip 控制结构体（静态=只本文件用） */
static struct netif g_lwip_netif;     /* 全局网络接口（lwip 的核心对象，代表"这块网卡"） */
static uint8_t  g_m2_eth_inited = 0;  /* 幂等保护：已初始化过就不再初始化（防止 main 里重复调用） */

/* DHCP 状态枚举（照例程 netconf.h，值别改，线程状态机靠它跳转）：
   OFF=没开始 → START=开始申请 → WAIT_ADDRESS=等分配 → ADDRESS_ASSIGNED=分配成功
   LINK_DOWN=网线断了 → TIMEOUT=申请超时（回落静态） */
#define LWIP_DHCP_OFF           0
#define LWIP_DHCP_START         1
#define LWIP_DHCP_WAIT_ADDRESS  2
#define LWIP_DHCP_ADDRESS_ASSIGNED 3
#define LWIP_DHCP_LINK_DOWN     4
#define LWIP_DHCP_TIMEOUT       5
static uint8_t g_lwip_dhcp_state = LWIP_DHCP_OFF;   /* 当前 DHCP 状态（全局，线程和链接回调都改它） */

#define LWIP_LINK_OFF           0   /* 断链 */
#define LWIP_LINK_ON            1   /* 已链接 */

/* DHCP 最大尝试次数：超过这个数还拿不到 IP 就放弃，回落静态 IP。
   例程原值 10（约 10 秒），太小容易被路由器慢启动误杀，太大等太久 */
#define LWIP_MAX_DHCP_TRIES     10

/* ================= 三、线程配置（照例程） ================= */
/* 链接线程：优先级 3、栈 128*2=256（单位是"字"=4 字节，即 1KB）。
   例程原值，够用：这线程就每秒读一次 PHY 寄存器，活很轻 */
#define LWIP_LINK_TASK_PRIO     3
#define LWIP_LINK_STK_SIZE      128 * 2

/* DHCP 线程：优先级 4（比链接线程高 1，因为拿 IP 更重要）、栈也是 256 字。
   为什么优先级都不高？因为 tcpip 内核线程优先级是 configMAX_PRIORITIES-2（最高档），
   协议栈处理不能被低优先级线程打断太久，所以业务线程都让着内核 */
#define LWIP_DHCP_TASK_PRIO     4
#define LWIP_DHCP_STK_SIZE      128 * 2

/* 函数声明（提前声明，避免编译器"隐式声明"告警） */
static void lwip_link_thread(void *argument);       /* 链接线程入口 */
static void lwip_dhcp_process(void *pvParameters);  /* DHCP 线程入口 */
static void lwip_link_status_updated(struct netif *netif);  /* 链接变化回调 */

/* ================= 四、默认静态 IP（DHCP 超时回落用） ================= */
static void lwip_comm_default_ip_set(__lwip_dev *lwipx)
{
  /* 电脑直连时的经典网段：板子 192.168.2.30，电脑在 192.168.2.x 同网段就能互 ping */
  lwipx->remoteip[0] = 192; lwipx->remoteip[1] = 168; lwipx->remoteip[2] = 0;   lwipx->remoteip[3] = 120;
  lwipx->ip[0]       = 192; lwipx->ip[1]       = 168; lwipx->ip[2]       = 2;   lwipx->ip[3]       = 30;
  lwipx->netmask[0]  = 255; lwipx->netmask[1]  = 255; lwipx->netmask[2]  = 255; lwipx->netmask[3]  = 0;
  lwipx->gateway[0]  = 192; lwipx->gateway[1]  = 168; lwipx->gateway[2]  = 2;   lwipx->gateway[3]  = 1;
  lwipx->dhcpstatus  = 0;   /* 初始没开 DHCP */
}

/* ================= 五、初始化入口：m2_ethernet_init() =================
 * 调用时机：main.c USER CODE 2 区（osKernelStart 之前）
 * 为什么在 osKernelStart 之前调？
 *   - tcpip_init 会创建 tcpip_thread（内核线程）
 *   - netif_add 内部 ethernetif_init 会创建收包线程、DHCP/链接线程在这里创建
 *   - FreeRTOS 允许"调度器启动前先创建任务"，创建完等 osKernelStart 一起跑
 * ==================================================================== */
int32_t m2_ethernet_init(void)
{
  struct netif *netif_init_flag = NULL;   /* netif_add 的返回值：判断网卡挂没挂上 */
  ip_addr_t ipaddr, netmask, gw;          /* 网卡的 IP/掩码/网关（netif_add 用） */

  if (g_m2_eth_inited)
    return ETH_OK;                        /* 已初始化过，直接返回（幂等） */

  /* 1. 初始化 tcpip 内核
     ★ 这一句内部创建了 lwip 的核心线程 tcpip_thread（名字 "lwip_thread"，栈 2048 字节），
       所有协议栈处理（TCP/UDP/ARP/DHCP 报文）都在这个线程里跑。
       参数 NULL,NULL = 不用初始化完成回调、不传自定义数据 */
  tcpip_init(NULL, NULL);

  /* 2. 先写好默认静态 IP（DHCP 超时回落要用），DHCP 成功会覆盖 g_lwipdev 里的值 */
  lwip_comm_default_ip_set(&g_lwipdev);

#if LWIP_DHCP
  /* 用 DHCP：网卡先给 0.0.0.0（IP 全 0），等 DHCP 线程去要地址。
     为什么给全 0？DHCP 流程要求客户端先以 0.0.0.0 发广播请求 */
  ipaddr.addr = 0;
  netmask.addr = 0;
  gw.addr = 0;
#else
  /* （不用 DHCP 才走这里：直接用静态 IP 配置网卡） */
  IP4_ADDR(&ipaddr, g_lwipdev.ip[0], g_lwipdev.ip[1], g_lwipdev.ip[2], g_lwipdev.ip[3]);
  IP4_ADDR(&netmask, g_lwipdev.netmask[0], g_lwipdev.netmask[1], g_lwipdev.netmask[2], g_lwipdev.netmask[3]);
  IP4_ADDR(&gw, g_lwipdev.gateway[0], g_lwipdev.gateway[1], g_lwipdev.gateway[2], g_lwipdev.gateway[3]);
#endif

  /* 3. 把网卡挂进 lwip
     参数依次是：
       &g_lwip_netif      —— 网卡对象（lwip 用这个结构代表本网卡）
       &ipaddr &netmask &gw —— 初始 IP/掩码/网关
       NULL               —— state：给驱动的自定义数据，不需要
       &ethernetif_init   —— ★初始化回调：netif_add 内部会调它，实际做硬件准备
                               （PHY 探测 + HAL_ETH_Start_IT + 建收包线程，见第 2 步 ethernetif.c）
       &tcpip_input       —— ★收包入口：驱动线程收到一帧就调 netif->input(=tcpip_input)
                               把包扔给内核线程处理
     ★ 外面套 taskENTER_CRITICAL 临界区：netif_add 会操作 lwip 内部链表，
       防止调度器还没起但其他线程乱入（照例程原样保留） */
  taskENTER_CRITICAL();
  netif_init_flag = netif_add(&g_lwip_netif, &ipaddr, &netmask, &gw,
                              NULL, &ethernetif_init, &tcpip_input);
  taskEXIT_CRITICAL();

  /* 网卡添加失败（内存不够等）：返回配置错误 */
  if (netif_init_flag == NULL)
  {
    g_m2_eth_inited = 0;                  /* 保持未初始化，允许重试 */
    return ETH_ERR_CONFIG;
  }

  /* 4. 设为默认网口：系统只有一个网卡，默认路由/ARP 都走它 */
  netif_set_default(&g_lwip_netif);

  /* 5. 链接回调 + 起链接线程
     先手动调一次 lwip_link_status_updated 初始化状态，
     再注册回调（网线插拔时 lwip 自动调它），最后建链接线程（每秒查 PHY） */
#if LWIP_NETIF_LINK_CALLBACK
  lwip_link_status_updated(&g_lwip_netif);
  netif_set_link_callback(&g_lwip_netif, lwip_link_status_updated);
  taskENTER_CRITICAL();
  sys_thread_new("eth_link", lwip_link_thread, &g_lwip_netif,
                 LWIP_LINK_STK_SIZE, LWIP_LINK_TASK_PRIO);
  taskEXIT_CRITICAL();
#endif

  /* 6. DHCP 线程：去路由器要 IP（超时自动回落静态）
     sys_thread_new = lwip 的建线程封装，带 OS 下由 sys_arch.c 实现（= FreeRTOS xTaskCreate） */
#if LWIP_DHCP
  g_lwipdev.dhcpstatus = 0;               /* 标记：还没 DHCP */
  g_lwip_dhcp_state = LWIP_DHCP_OFF;      /* 状态机复位 */
  taskENTER_CRITICAL();
  sys_thread_new("eth_dhcp", lwip_dhcp_process, &g_lwip_netif,
                 LWIP_DHCP_STK_SIZE, LWIP_DHCP_TASK_PRIO);
  taskEXIT_CRITICAL();
#endif

  g_lwipdev.link_status = LWIP_LINK_OFF;  /* 初始标记断链（链接线程马上会更新） */
  g_m2_eth_inited = 1;                    /* 标记已初始化 */
  return ETH_OK;
}

/* ================= 六、链路等待 / 轮询接口 ================= */

/**
 * @brief  等待链路建立（网线插好 + PHY 已链接）
 * @param  timeout_ms 超时毫秒（0=无限等待）
 * @retval ETH_OK 已链接；ETH_ERR_LINK 超时
 * @note   谁用？以后的 mqtt 服务连服务器前先等这个，保证网络就绪
 */
int32_t m2_ethernet_link_wait(uint32_t timeout_ms)
{
  uint32_t elapsed = 0;                   /* 已等待时间（毫秒累计） */

  /* 循环查 lwip 的"链路已链接"标志：
     netif_is_link_up 由链接线程/回调更新（lwip 内部维护），不是我们直接置的 */
  while (netif_is_link_up(&g_lwip_netif) == 0)
  {
    if ((timeout_ms != 0) && (elapsed >= timeout_ms))
      return ETH_ERR_LINK;                /* 超时：网线没插好或对端没起来 */
    vTaskDelay(100 / portTICK_PERIOD_MS); /* 睡 100ms 再查（别空转烧 CPU） */
    elapsed += 100;
  }
  return ETH_OK;                          /* 链路好了 */
}

/** 带 OS 版空实现：收包由 ethernetif_thread（第 2 步）处理。
    保留这个函数是为了跟 Day1 骨架的接口约定兼容（以后 mqtt 层调用不报错） */
void m2_ethernet_poll(void)
{
}

/* ================= 七、链接状态更新回调 =================
 * 被 lwip 自动调用（注册在 netif_set_link_callback），
 * 触发时机：netif_set_link_up/down、netif_set_up/down 时。
 * 作用：网线插拔时通知 DHCP 状态机去启动/停止申请 */
static void lwip_link_status_updated(struct netif *netif)
{
  if (netif_is_up(netif))                 /* 网口处于 up 状态 = 网络就绪 */
  {
#if LWIP_DHCP
    g_lwip_dhcp_state = LWIP_DHCP_START;  /* 让 DHCP 线程进入"开始申请"状态 */
    printf("The network cable is connected\r\n");
#endif
  }
  else                                    /* 网口 down = 断线 */
  {
#if LWIP_DHCP
    g_lwip_dhcp_state = LWIP_DHCP_LINK_DOWN;  /* 让 DHCP 线程去停掉申请 */
    printf("The network cable is not connected\r\n");
#endif
  }
}

/* ================= 八、链接线程 =================
 * 每秒检查一次 PHY 链接状态（网线插没插），变化时：
 *   - 断线：停掉 ETH（DMA 停止收包）+ 网卡标记 down
 *   - 插线：重启 ETH（DMA + 接收中断）+ 网卡标记 up
 * 为什么要自己盯 PHY 而不是靠中断？RMII 没有"链接中断"引脚给 MCU，
 * 只能用轮询方式读 PHY 状态寄存器 */
static void lwip_link_thread(void *argument)
{
  struct netif *netif = (struct netif *)argument;  /* 线程参数 = 网卡对象 */
  int link_again_num = 0;   /* ★去抖计数器：连续 2 次读到"断线"才当真 */
                            /* 为什么去抖？网线接触不良/交换机重启会瞬间闪断，
                               不处理，PHY 状态会在 1 秒内抖动，反复启停 ETH 反而搞挂协议栈 */

  for (;;)
  {
    /* 读 PHY 链接状态：1=网线插好且对端活着，0=没插/对端没电 */
    if (lan8720_link_read() == 0)
    {
      g_lwipdev.link_status = LWIP_LINK_OFF;   /* 记录断链（业务层能查） */
      link_again_num++;                        /* 断线计数 +1 */

      if (link_again_num >= 2)
        continue;   /* 已经连续 2 秒断线：上轮已处理过，直接跳到下一轮，别再重复关 */
      else          /* 第一次读到断线：真正执行"断线处理" */
      {
#if LWIP_DHCP
        g_lwip_dhcp_state = LWIP_DHCP_LINK_DOWN;  /* 通知 DHCP 线程停止申请 */
        dhcp_stop(netif);                         /* 停掉 DHCP（释放租约） */
#endif
        HAL_ETH_Stop(&heth);        /* ★停掉 ETH 硬件：DMA 停止，不再接收新帧 */
        netif_set_down(netif);      /* 网口标记 down（lwip 不对外提供服务） */
        netif_set_link_down(netif); /* 链路标记 down（触发上面注册的链接回调） */
      }
    }
    else  /* 网线插入（或恢复链接） */
    {
      link_again_num = 0;                       /* 清零去抖计数 */
      if (g_lwipdev.link_status == LWIP_LINK_OFF)  /* 之前是断的 → 现在好了，要"重新启动" */
      {
        g_lwipdev.link_status = LWIP_LINK_ON;   /* 记录已链接 */
        HAL_ETH_Start_IT(&heth);    /* ★重启 ETH：DMA + 接收中断都开，恢复收包 */
        netif_set_up(netif);        /* 网口标记 up */
        netif_set_link_up(netif);   /* 链路标记 up（触发回调 → DHCP 重新申请） */
      }
      /* 若本来就连着，什么都不做 */
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);      /* 每秒轮询一次（去抖粒度就是 1 秒） */
  }
}

/* ================= 九、DHCP 线程：状态机 =================
 * 状态流转（照例程 netconf.c）：
 *   START → WAIT_ADDRESS →（拿到 IP）ADDRESS_ASSIGNED
 *                       →（tries 超 10 次）TIMEOUT：停 DHCP，回落静态 IP
 *   LINK_DOWN（网线断）→ 停 DHCP → OFF，等下次插线回调再 START
 * 每秒循环一次，状态由链接回调和其他分支改写 */
#if LWIP_DHCP
static void lwip_dhcp_process(void *pvParameters)
{
  struct netif *netif = (struct netif *)pvParameters;  /* 线程参数 = 网卡对象 */
  uint32_t ip = 0, netmask = 0, gw = 0;               /* 缓存从 netif 读出的地址（解析字节用） */
  struct dhcp *dhcp;                                  /* DHCP 客户端内部状态（看尝试次数） */

  g_lwipdev.dhcpstatus = 1;   /* 1=正在向 DHCP 服务器申请 */

  for (;;)
  {
    switch (g_lwip_dhcp_state)   /* 按当前状态执行对应动作 */
    {
      case LWIP_DHCP_START:
      {
        /* 清零网卡上的地址（回到 0.0.0.0），这是 DHCP 申请的前置条件 */
        ip_addr_set_zero_ip4(&netif->ip_addr);
        ip_addr_set_zero_ip4(&netif->netmask);
        ip_addr_set_zero_ip4(&netif->gw);
        g_lwip_dhcp_state = LWIP_DHCP_WAIT_ADDRESS;   /* 进入"等分配"状态 */
        dhcp_start(netif);        /* ★真正发起 DHCP 请求（发广播包） */
        printf("Looking for DHCP server, please wait...\r\n");
      }
      break;

      case LWIP_DHCP_WAIT_ADDRESS:
      {
        /* 每次循环把当前地址读出来（DHCP 成功时 lwip 会写进 netif） */
        ip      = g_lwip_netif.ip_addr.addr;
        netmask = g_lwip_netif.netmask.addr;
        gw      = g_lwip_netif.gw.addr;

        /* ★判断是否已经拿到 IP：lwip 内部标记"DHCP 给了地址" */
        if (dhcp_supplied_address(netif))
        {
          g_lwip_dhcp_state = LWIP_DHCP_ADDRESS_ASSIGNED;  /* 进入稳定态（不再重复申请） */
          if (ip != 0)   /* 地址非 0 才是真的拿到了 */
          {
            g_lwipdev.dhcpstatus = 2;    /* 2=DHCP 成功 */

            /* 把 netif 里的 32 位地址拆成 4 字节存进 g_lwipdev 打印用。
               ★注意字节顺序：lwip 存的是大端（网络字节序），
               ip >> 24 取出的是最高字节 = 点分十进制第一段（如 192） */
            g_lwipdev.ip[3]      = (uint8_t)(ip >> 24);
            g_lwipdev.ip[2]      = (uint8_t)(ip >> 16);
            g_lwipdev.ip[1]      = (uint8_t)(ip >> 8);
            g_lwipdev.ip[0]      = (uint8_t)(ip);
            g_lwipdev.netmask[3] = (uint8_t)(netmask >> 24);
            g_lwipdev.netmask[2] = (uint8_t)(netmask >> 16);
            g_lwipdev.netmask[1] = (uint8_t)(netmask >> 8);
            g_lwipdev.netmask[0] = (uint8_t)(netmask);
            g_lwipdev.gateway[3] = (uint8_t)(gw >> 24);
            g_lwipdev.gateway[2] = (uint8_t)(gw >> 16);
            g_lwipdev.gateway[1] = (uint8_t)(gw >> 8);
            g_lwipdev.gateway[0] = (uint8_t)(gw);
            printf("DHCP assigned: %d.%d.%d.%d / %d.%d.%d.%d / gw %d.%d.%d.%d\r\n",
                   g_lwipdev.ip[0], g_lwipdev.ip[1], g_lwipdev.ip[2], g_lwipdev.ip[3],
                   g_lwipdev.netmask[0], g_lwipdev.netmask[1], g_lwipdev.netmask[2], g_lwipdev.netmask[3],
                   g_lwipdev.gateway[0], g_lwipdev.gateway[1], g_lwipdev.gateway[2], g_lwipdev.gateway[3]);
          }
        }
        else
        {
          /* 还没拿到：看尝试次数是否超限（超过 LWIP_MAX_DHCP_TRIES 次） */
          dhcp = (struct dhcp *)netif_get_client_data(netif, LWIP_NETIF_CLIENT_DATA_INDEX_DHCP);
          if (dhcp->tries > LWIP_MAX_DHCP_TRIES)
          {
            /* ★DHCP 超时：放弃动态分配，回落静态 IP */
            printf("DHCP timeout, fallback to static IP...\r\n");
            g_lwip_dhcp_state = LWIP_DHCP_TIMEOUT;  /* 进入超时态（不再重复尝试） */
            dhcp_stop(netif);                        /* 停掉 DHCP */
            g_lwipdev.dhcpstatus = 0xFF;             /* 0xFF=正在用静态 */

            /* 把默认静态 IP 直接写进网卡 */
            IP4_ADDR(&(g_lwip_netif.ip_addr),  g_lwipdev.ip[0], g_lwipdev.ip[1], g_lwipdev.ip[2], g_lwipdev.ip[3]);
            IP4_ADDR(&(g_lwip_netif.netmask), g_lwipdev.netmask[0], g_lwipdev.netmask[1], g_lwipdev.netmask[2], g_lwipdev.netmask[3]);
            IP4_ADDR(&(g_lwip_netif.gw),      g_lwipdev.gateway[0], g_lwipdev.gateway[1], g_lwipdev.gateway[2], g_lwipdev.gateway[3]);
            netif_set_addr(netif, &g_lwip_netif.ip_addr, &g_lwip_netif.netmask, &g_lwip_netif.gw);
            printf("Static IP: %d.%d.%d.%d / %d.%d.%d.%d / gw %d.%d.%d.%d\r\n",
                   g_lwipdev.ip[0], g_lwipdev.ip[1], g_lwipdev.ip[2], g_lwipdev.ip[3],
                   g_lwipdev.netmask[0], g_lwipdev.netmask[1], g_lwipdev.netmask[2], g_lwipdev.netmask[3],
                   g_lwipdev.gateway[0], g_lwipdev.gateway[1], g_lwipdev.gateway[2], g_lwipdev.gateway[3]);
          }
        }
      }
      break;

      case LWIP_DHCP_LINK_DOWN:
      {
        /* 网线断了：停掉 DHCP，回到 OFF；下次插线回调会置 START 重新申请 */
        dhcp_stop(netif);
        g_lwip_dhcp_state = LWIP_DHCP_OFF;
      }
      break;

      default:
        break;   /* ADDRESS_ASSIGNED / TIMEOUT / OFF：稳定态，什么都不做 */
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);   /* 每秒跑一次状态机（DHCP 响应本身也是秒级） */
  }
}
#endif /* LWIP_DHCP */
