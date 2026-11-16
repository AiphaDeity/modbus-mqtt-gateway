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

// ===== 一、头文件，每个都有自己的用处 =====
#include "lwip/mem.h"        // LwIP堆内存API，netif体系内部需要，本文件没直接调
#include "lwip/memp.h"       // LwIP内存池API，netif内部依赖它
#include "lwip/tcp.h"        // TCP协议控制块类型，netif关联TCP需要
#include "lwip/tcpip.h"      // 核心：tcpip_init创建内核线程，tcpip_input是喂包入口
#include "lwip/udp.h"        // UDP控制块类型
#include "netif/etharp.h"    // ARP模块，ethernetif里发数据要用etharp_output
#include "lwip/dhcp.h"       // DHCP：开始、停止、判断有没有拿到IP
#include "lwip/dns.h"        // DNS：静态IP兜底时设置公共DNS服务器
#include "lwip/init.h"       // LwIP初始化，tcpip_init内部会调，不用自己调
#include "lwip/timeouts.h"   // LwIP超时管理，tcpip线程内部用
#include "lwip/netif.h"      // 核心：netif结构体，添加网卡、设默认、判断链路状态
#include "lwip/netifapi.h"   // 线程安全封装：带OS时改netif状态必须经它投递到内核线程，不能直接调

#include "FreeRTOS.h"        // FreeRTOS主头，临界区宏在这里
#include "task.h"            // 任务相关API，vTaskDelay延时函数

#include "stm32f4xx_hal.h"   // HAL主头，操作ETH硬件的启停
#include <stdio.h>           // printf打印，main.c已经重定向好了，直接能用

#include "ethernet.h"        // 本模块对外接口
#include "ethernetif.h"      // 网卡驱动：初始化回调、读PHY链路状态

// 同事 Core/Src/eth.c 里的 ETH 硬件句柄，这里只是借用，不是重复定义
// 链接线程里要操作硬件启停，必须用这个句柄，所以用 extern 声明
extern ETH_HandleTypeDef heth;                     // 引用CubeMX生成的ETH句柄，链接线程操作硬件用

// ===== 二、全局控制结构，照探索者例程改的 =====
// LwIP控制结构体，记录本机IP、掩码、网关和各种状态，打印和DHCP回落用
typedef struct                                   // LwIP控制结构体定义开始
{
  uint8_t  remoteip[4];   // 远端IP，例程测试用的，M2暂时用不到，保留字段
  uint8_t  ip[4];         // 本机IP，DHCP成功就更新，超时回落静态IP
  uint8_t  netmask[4];    // 子网掩码，默认255.255.255.0
  uint8_t  gateway[4];    // 默认网关，默认192.168.1.1
  uint8_t  dhcpstatus;    // DHCP状态：0没用、1请求中、2成功、0xFF超时回落
  uint8_t  link_status;   // 链路状态：0断链、1已连接，链接线程更新
} __lwip_dev;                                    // 结构体别名__lwip_dev定义结束

static __lwip_dev g_lwipdev;          // 控制结构体实例，static只本文件能用
static struct netif g_lwip_netif;     // LwIP网络接口对象，代表这块网卡，核心对象
static uint8_t  g_m2_eth_inited = 0;  // 幂等保护：初始化过就不再初始化，防止重复调用
static uint8_t g_tcpip_started;       // tcpip内核有没有启动的标志
static volatile uint8_t g_tcpip_ready; // 内核是否就绪，volatile防优化
static uint8_t g_netif_added;         // 网卡有没有添加成功的标志
#if LWIP_NETIF_LINK_CALLBACK                    // 开了链路回调编译才保留链接线程句柄
static sys_thread_t g_link_thread;    // 链接线程句柄，开了链路回调才用
#endif                                           // 条件编译结束
#if LWIP_DHCP                                    // 开了DHCP编译才保留DHCP线程句柄
static sys_thread_t g_dhcp_thread;    // DHCP线程句柄，开了DHCP才用
#endif                                           // 条件编译结束

// DHCP状态枚举，线程状态机靠这些值跳转，值不能随便改
// OFF没开始 → START开始申请 → WAIT等分配 → ASSIGNED成功 → LINK_DOWN断网 → TIMEOUT超时
#define LWIP_DHCP_OFF           0                // 状态0：没开始DHCP
#define LWIP_DHCP_START         1                // 状态1：开始申请
#define LWIP_DHCP_WAIT_ADDRESS  2                // 状态2：等服务器分配IP
#define LWIP_DHCP_ADDRESS_ASSIGNED 3             // 状态3：IP分配成功
#define LWIP_DHCP_LINK_DOWN     4                // 状态4：网线断了
#define LWIP_DHCP_TIMEOUT       5                // 状态5：超时放弃
static uint8_t g_lwip_dhcp_state = LWIP_DHCP_OFF;   // 当前DHCP状态，线程和回调都会改它

#define LWIP_LINK_OFF           0   // 断链
#define LWIP_LINK_ON            1   // 已连接

// DHCP最大尝试次数，超过还拿不到IP就放弃，回落静态IP
// 调试临时改小成2，直连PC没DHCP服务器能快速超时；正式环境要恢复成10
#define LWIP_MAX_DHCP_TRIES     2                // DHCP最多试2次（临时值，正式改10）

// ===== 三、线程配置，照探索者例程改的 =====
// 链接线程栈大小：768字=3KB，因为有printf打印，1KB栈会爆
// 优先级24，和Storage/MQTT同级，之前太低会被业务任务饿死，永远调度不到
#define LWIP_LINK_TASK_PRIO     24               // 链接线程优先级24，和业务同级防饿死
#define LWIP_LINK_STK_SIZE      128 * 6          // 链接线程栈768字=3KB，printf需要大栈

// DHCP线程栈也是3KB，发DHCP请求调用链很深，1KB栈会爆
// 优先级同样24，避免被业务任务饿死；tcpip内核线程优先级最高，业务线程都让着它
#define LWIP_DHCP_TASK_PRIO     24               // DHCP线程优先级24，和业务同级防饿死
#define LWIP_DHCP_STK_SIZE      128 * 6          // DHCP线程栈768字=3KB，调用链深需要大栈

// 函数提前声明，避免编译器报隐式声明警告
static void lwip_link_thread(void *argument);       // 链接线程入口函数
static void lwip_dhcp_process(void *pvParameters);  // DHCP线程入口函数
static void lwip_link_status_updated(struct netif *netif);  // 链路变化回调函数

// ===== 四、默认静态IP，DHCP超时回落用 =====
// 函数：lwip_comm_default_ip_set —— 设置默认静态IP
// 功能：DHCP反复申请失败或超时后，把网卡的IP/子网掩码/网关设成固定静态值（192.168.2.121网段那套），保证拿不到DHCP也能在局域网通信，设备不会彻底失联
// 参数：lwipx —— LwIP设备结构指针，里面的ip/nums等字段会被填上默认静态地址
// 返回：void，无返回值，纯结构体赋值
static void lwip_comm_default_ip_set(__lwip_dev *lwipx)   // 设置默认静态IP，DHCP超时回落用
{
  // 静态IP末字节本来要从芯片96位UID派生，防止多块板回落时撞IP
  // 三个UID地址都是STM32出厂固化的，只读，异或混合一下
  uint32_t u = *(volatile uint32_t *)0x1FFF7A10 ^ *(volatile uint32_t *)0x1FFF7A14 ^ *(volatile uint32_t *)0x1FFF7A18;  // 三个UID字异或混合成主机号种子
  uint8_t host = (uint8_t)((u & 0xFFU) ^ ((u >> 8) & 0xFFU));  // 取低两个字节再异或
  if (host < 2)   host = 2;                    // 主机号不能是0或1，最小2
  if (host > 254) host = 254;                  // 最大254，255是广播地址
  host = 124;  // 调试临时固定成124，方便直连验证；正式环境要删掉这行恢复UID派生

  lwipx->remoteip[0] = 192; lwipx->remoteip[1] = 168; lwipx->remoteip[2] = 0;   lwipx->remoteip[3] = 120;  // 远端IP，测试用
  lwipx->ip[0]       = 192; lwipx->ip[1]       = 168; lwipx->ip[2]       = 2;    lwipx->ip[3]       = host;  // 本机IP，192.168.2.x
  lwipx->netmask[0]  = 255; lwipx->netmask[1]  = 255; lwipx->netmask[2]  = 255; lwipx->netmask[3]  = 0;     // 子网掩码24位
  lwipx->gateway[0]  = 192; lwipx->gateway[1]  = 168; lwipx->gateway[2]  = 2;   lwipx->gateway[3]  = 1;     // 网关192.168.2.1
  lwipx->dhcpstatus  = 0;                      // 初始没开DHCP，状态0
}

/* ================= 五、初始化入口 ================= */
// 函数：lwip_init_done —— tcpip内核就绪回调
// 功能：tcpip_init初始化LwIP内核线程完成后LwIP自动调这个回调，把内核就绪信号量释放掉，让m2_ethernet_init里等待的代码继续往下跑，确认协议栈真正可用
// 参数：argument —— 回调上下文参数，本回调没用到
// 返回：void，无返回值，作用是释放就绪信号量
static void lwip_init_done(void *argument)      // tcpip内核线程就绪回调
{
  (void)argument;                              // 没用到参数，显式忽略
  g_tcpip_ready = 1;                           // 把全局就绪标志置1
}

// 函数：m2_ethernet_init —— 以太网初始化主入口（对外接口）
// 功能：以太网全部初始化的总入口，只由启动任务串行调用：初始化MAC和PHY（LAN8720A）→起tcpip内核线程并等就绪→挂网卡netif→建链接状态回调→起DHCP申请线程和PHY轮询线程→标记初始化完成；已初始化过直接返回成功，单项失败下轮可重试补缺
// 参数：无（操作全局LwIP设备结构和网卡状态）
// 返回：int32_t，0代表初始化全部完成；负数错误码代表某一步失败（PHY没响应/DHCP线程没建起来等），可稍后重试
int32_t m2_ethernet_init(void)                   // 以太网初始化主入口，启动任务调用
{
  ip_addr_t ipaddr, netmask, gw;               // 临时IP地址变量，添加网卡时用

  if (g_m2_eth_inited)                           // 幂等保护判断
    return ETH_OK;                             // 已经初始化过了直接返回成功，幂等保护
  if (heth.gState == HAL_ETH_STATE_RESET || heth.gState == HAL_ETH_STATE_ERROR)  // HAL句柄没就绪或出错
  {
    printf("[ETH] HAL not ready: state=%u\r\n", (unsigned)heth.gState);  // HAL硬件还没就绪
    return ETH_ERR_CONFIG;                     // 返回配置错误
  }

  if (!g_tcpip_started)                          // 内核还没启动
  {                                               // 分支体开始
    g_tcpip_started = 1;                       // 标记内核启动中
    tcpip_init(lwip_init_done, NULL);          // 启动LwIP内核线程，传就绪回调
  }                                             // 分支体结束
  uint32_t started_ms = HAL_GetTick();         // 记录开始等待的时间
  while (!g_tcpip_ready)                        // 等内核就绪标志置1
  {                                             // 等待循环开始
    if ((uint32_t)(HAL_GetTick() - started_ms) >= 2000U)  // 等够2秒还没就绪
    {                                           // 超时分支开始
      printf("[ETH] tcpip init timeout\r\n");   // 等2秒还没就绪，超时
      return ETH_ERR_CONFIG;                    // 超时返回配置错误
    }                                           // 超时分支结束
    vTaskDelay(pdMS_TO_TICKS(10));              // 没就绪就等10毫秒再看，不占CPU
  }                                             // 等待循环结束

  if (!g_netif_added)                            // 网卡还没添加
  {                                             // 分支体开始
    lwip_comm_default_ip_set(&g_lwipdev);       // 先设置默认静态IP参数
#if LWIP_DHCP                                   // 开了DHCP走这个分支
    ip_addr_set_zero_ip4(&ipaddr);              // 开了DHCP就先把IP清零，等DHCP分配
    ip_addr_set_zero_ip4(&netmask);             // 掩码也清零
    ip_addr_set_zero_ip4(&gw);                  // 网关也清零
#else                                           // 没开DHCP走静态IP分支
    // 没开DHCP就用静态IP，把四个字节拼成IP地址
    IP4_ADDR(&ipaddr, g_lwipdev.ip[0], g_lwipdev.ip[1], g_lwipdev.ip[2], g_lwipdev.ip[3]);   // 拼本机IP
    IP4_ADDR(&netmask, g_lwipdev.netmask[0], g_lwipdev.netmask[1], g_lwipdev.netmask[2], g_lwipdev.netmask[3]);  // 拼掩码
    IP4_ADDR(&gw, g_lwipdev.gateway[0], g_lwipdev.gateway[1], g_lwipdev.gateway[2], g_lwipdev.gateway[3]);  // 拼网关
#endif                                          // 条件编译结束
    // 线程安全地添加网卡，ethernetif_init是网卡初始化回调，tcpip_input是收包入口
    if (netifapi_netif_add(&g_lwip_netif, &ipaddr, &netmask, &gw,   // 添加网卡到LwIP
                          NULL, ethernetif_init, tcpip_input) != ERR_OK)  // 不带state，初始化回调和收包入口
    {                                           // 失败分支开始
      printf("[ETH] netif init failed\r\n");    // 网卡添加失败
      return ETH_ERR_CONFIG;                    // 返回配置错误
    }                                           // 失败分支结束
    g_netif_added = 1;                         // 标记网卡已添加
#if LWIP_NETIF_LINK_CALLBACK                    // 开了链路回调才注册
    LOCK_TCPIP_CORE();                           // 锁住内核，直接操作netif
    lwip_link_status_updated(&g_lwip_netif);     // 先读一次当前链路状态
    netif_set_link_callback(&g_lwip_netif, lwip_link_status_updated);  // 注册链路变化回调
    UNLOCK_TCPIP_CORE();                         // 解锁内核
#endif                                          // 条件编译结束
  }                                             // 分支体结束

  if (netifapi_netif_set_default(&g_lwip_netif) != ERR_OK)  // 设默认网卡失败判断
    return ETH_ERR_CONFIG;                       // 把这块网卡设为默认网卡，失败返回错误

#if LWIP_NETIF_LINK_CALLBACK                    // 开了链路回调才创建链接线程
  if (g_link_thread.thread_handle == NULL)      // 链接线程还没创建过
  {                                             // 分支体开始
    // 创建链接线程，名字叫eth_link，入口lwip_link_thread，传网卡对象
    g_link_thread = sys_thread_new("eth_link", lwip_link_thread, &g_lwip_netif,   // 创建链接线程
                                   LWIP_LINK_STK_SIZE, LWIP_LINK_TASK_PRIO);   // 栈3KB优先级24
    if (g_link_thread.thread_handle == NULL)    // 句柄还是空说明创建失败
      return ETH_ERR_CONFIG;                     // 线程创建失败，内存不够
  }                                             // 分支体结束
#endif                                          // 条件编译结束
#if LWIP_DHCP                                   // 开了DHCP才创建DHCP线程
  if (g_dhcp_thread.thread_handle == NULL)      // DHCP线程还没创建过
  {                                             // 分支体开始
    // 创建DHCP线程，名字叫eth_dhcp，入口lwip_dhcp_process
    g_dhcp_thread = sys_thread_new("eth_dhcp", lwip_dhcp_process, &g_lwip_netif,   // 创建DHCP线程
                                   LWIP_DHCP_STK_SIZE, LWIP_DHCP_TASK_PRIO);   // 栈3KB优先级24
    if (g_dhcp_thread.thread_handle == NULL)    // 句柄还是空说明创建失败
      return ETH_ERR_CONFIG;                     // 创建失败
  }                                             // 分支体结束
#endif                                          // 条件编译结束

  g_m2_eth_inited = 1;                           // 全部初始化成功，标记已初始化
  return ETH_OK;                                 // 返回成功
}                                               // 初始化函数结束

// ===== 六、链路等待和轮询接口 =====

// 函数：m2_ethernet_link_wait —— 等待物理链路建立（对外接口）
// 功能：给MQTT等上层调的接口：阻塞等待网线插好、PHY协商成功链路up，期间周期性检查不占死CPU；MQTT连broker前先调这个确认物理层通，避免白发连接
// 参数：timeout_ms —— 最长等待毫秒数，传0表示无限等下去直到链路通
// 返回：int32_t，0代表链路已建立；ERR_LINK超时（规定时间内链路一直没通）
int32_t m2_ethernet_link_wait(uint32_t timeout_ms)   // 等链路建立，MQTT连接前调用
{
  if (!g_m2_eth_inited)                          // 初始化没做过
    return ETH_ERR_CONFIG;                  // 还没初始化，直接返回错误
  uint32_t elapsed = 0;                    // 已经等了多少毫秒

  // 循环查LwIP的链路标志，这个标志由链接线程/回调更新，不是自己直接改
  while (netif_is_link_up(&g_lwip_netif) == 0)   // 链路还是断的
  {                                         // 等待循环开始
    if ((timeout_ms != 0) && (elapsed >= timeout_ms))   // 有限时且到点了
      return ETH_ERR_LINK;                 // 超时了还没连上，返回链路错误
    vTaskDelay(100 / portTICK_PERIOD_MS);  // 睡100毫秒再查，不空转烧CPU
    elapsed += 100;                         // 累计等待时间
  }                                         // 等待循环结束
  return ETH_OK;                           // 链路好了，返回成功
}                                           // 链路等待函数结束

// 函数：m2_ethernet_poll —— 空轮询（接口兼容用）
// 功能：带OS版本下的空实现：收包实际由ethernetif网卡线程在后台自动处理，不需要任何人手动轮询；保留这个函数只为兼容无OS裸机版本的接口形状，别的模块调了也不做任何事
// 参数：无
// 返回：void，无返回值，函数体是空的
void m2_ethernet_poll(void)                      // 空轮询，接口兼容用
{
}

// ===== 七、链接状态更新回调 =====
// 函数：lwip_link_status_updated —— 链路状态变化回调
// 功能：LwIP在网线插拔链路up/down变化时自动调用（注册在netif链路回调）：通知DHCP状态机跟随链路——插线就从OFF重新开始申请IP，拔线就停申请并清状态，防止拔线后DHCP状态机空转
// 参数：netif —— 发生链路变化的网卡结构指针，从中判断链路是up还是down
// 返回：void，无返回值
static void lwip_link_status_updated(struct netif *netif)   // 链路变化回调
{
  if (netif_is_up(netif))                 // 网口up状态，网络就绪
  {
#if LWIP_DHCP                           // 开了DHCP才有这逻辑
    g_lwip_dhcp_state = LWIP_DHCP_START;  // 让DHCP线程进入开始申请状态
    printf("The network cable is connected\r\n");   // 打印网线已连接
#endif                                    // 条件编译结束
  }
  else                                    // 网口down，断线了
  {
#if LWIP_DHCP                           // 开了DHCP才有这逻辑
    g_lwip_dhcp_state = LWIP_DHCP_LINK_DOWN;  // 让DHCP线程停掉申请
    printf("The network cable is not connected\r\n");   // 打印网线断了
#endif                                    // 条件编译结束
  }
}                                         // 回调函数结束

// ===== 八、链接线程，每秒轮询一次PHY状态 =====
// 函数：lwip_link_thread —— 链接状态轮询线程
// 功能：FreeRTOS线程入口，每1秒读一次PHY的BSR寄存器（bit2链路状态）：链路从断到通就标记netif链路up并打日志，从通到断就标记down；RMII接口没有专用链路中断脚，只能这样轮询感知网线插拔
// 参数：argument —— FreeRTOS线程参数，没用到
// 返回：void，无返回值，死循环不退出
static void lwip_link_thread(void *argument)    // 链接线程入口，每秒轮询PHY
{
  struct netif *netif = (struct netif *)argument;  // 线程参数就是网卡对象
  int link_again_num = 0;   // 去抖计数器，连续2次读到断线才当真，防止闪断抖动

  for (;;)                                  // 线程死循环
  {                                         // 循环体开始
    // 读PHY链路状态：1=插好且对端活着，0=没插或者对端没电
    if (lan8720_link_read() == 0)           // PHY报链路断
    {                                       // 断线分支开始
      g_lwipdev.link_status = LWIP_LINK_OFF;   // 记录断链，业务层可以查
      link_again_num++;                        // 断线计数加1

      if (link_again_num >= 2)                 // 连续断线
      {                                       // 分支体开始
        // 连续2秒断线，上一轮已经处理过了，这里什么都不做，不能continue，会跳过末尾延时
      }                                       // 分支体结束
      else          // 第一次读到断线，真正执行断线处理
      {                                       // 分支体开始
#if LWIP_DHCP                                // 开了DHCP才通知
        g_lwip_dhcp_state = LWIP_DHCP_LINK_DOWN;  // 通知DHCP线程停止申请
#endif                                       // 条件编译结束
        // 不调HAL_ETH_Stop，实测停了再启动会导致TX描述符状态不一致，只停netif层就行
        // 改netif状态必须用netifapi线程安全封装，投递到内核线程执行，不能直接调
        netifapi_netif_set_down(netif);     // 网卡标记down，走内核线程
        netifapi_netif_set_link_down(netif);  // 链路标记断开
        netifapi_dhcp_stop(netif);          // 停掉DHCP申请
      }                                      // 分支体结束
    }                                        // 断线分支结束
    else  // 网线插入，或者链路恢复了
    {                                       // 恢复分支开始
      link_again_num = 0;                       // 清零去抖计数器
      if (g_lwipdev.link_status == LWIP_LINK_OFF)  // 之前是断的，现在好了，要重新启动
      {                                     // 分支体开始
        g_lwipdev.link_status = LWIP_LINK_ON;   // 记录已连接
        // 不调HAL_ETH_Start_IT，MAC本来就一直在跑，重复启动会搞坏TX描述符，只恢复netif层
        netifapi_netif_set_up(netif);        // 线程安全地标记网卡up，走内核线程
        netifapi_netif_set_link_up(netif);   // 链路标记连上
      }                                     // 分支体结束
      // 本来就连着的话什么都不做
    }                                        // 恢复分支结束

    vTaskDelay(1000 / portTICK_PERIOD_MS);      // 睡1秒，每秒轮询一次，去抖粒度就是1秒
  }                                          // 死循环结束
}                                            // 链接线程结束

// ===== 九、DHCP线程，状态机，每秒循环一次 =====
// 状态流转：START开始 → WAIT等分配 → ASSIGNED成功，或者TIMEOUT超时回落静态IP
// 网线断了就LINK_DOWN → OFF，等下次插线再重新开始
#if LWIP_DHCP                                   // 开了DHCP才编译这个线程
// 函数：lwip_dhcp_process —— DHCP申请线程
// 功能：FreeRTOS线程入口，跑DHCP状态机：START开始→WAIT等DHCP服务器分配（期间每250ms轮询一次）→ASSIGNED拿到IP打日志→网线断走LINK_DOWN清状态回OFF等下次插线；反复超时就调lwip_comm_default_ip_set回落静态IP
// 参数：pvParameters —— FreeRTOS线程参数，没用到
// 返回：void，无返回值，死循环不退出
static void lwip_dhcp_process(void *pvParameters)   // DHCP线程入口，状态机
{
  printf("[DHCP] thread started, state=%d freeHeap=%u\r\n", g_lwip_dhcp_state, (unsigned)xPortGetFreeHeapSize());  // 线程启动日志，打印状态和剩余堆
  struct netif *netif = (struct netif *)pvParameters;  // 线程参数就是网卡对象
  uint32_t ip = 0, netmask = 0, gw = 0;               // 缓存读出来的地址，打印用
  struct dhcp *dhcp;                                  // DHCP内部状态结构，看尝试次数

  g_lwipdev.dhcpstatus = 1;   // 1=正在向DHCP服务器申请

  for (;;)                                  // 线程死循环
  {                                         // 循环体开始
    switch (g_lwip_dhcp_state)   // 按当前状态执行对应动作
    {                                       // 状态机开始
      case LWIP_DHCP_START:                 // 开始申请状态
      {                                     // case体开始
        // 先把网卡地址清零，回到0.0.0.0，这是DHCP申请的前置条件，必须走netifapi线程安全
        ip_addr_t zero;                     // 临时清零地址
        ip_addr_set_zero_ip4(&zero);        // 填成0.0.0.0
        netifapi_netif_set_addr(netif, &zero, &zero, &zero);   // 清零网卡三地址
        g_lwip_dhcp_state = LWIP_DHCP_WAIT_ADDRESS;   // 进入等分配状态
        netifapi_dhcp_start(netif);   // 真正发起DHCP广播请求，在内核线程执行
        printf("Looking for DHCP server, please wait...\r\n");   // 打印正在找DHCP服务器
      }
      break;                                // 跳出switch，开始申请动作完成

      case LWIP_DHCP_WAIT_ADDRESS:           // 等分配状态
      {                                       // case体开始
        // 每次循环先把当前地址读出来，DHCP成功了LwIP会自动写进netif
        ip      = g_lwip_netif.ip_addr.addr;       // 读当前IP
        netmask = g_lwip_netif.netmask.addr;       // 读当前掩码
        gw      = g_lwip_netif.gw.addr;            // 读当前网关

        // 判断LwIP是不是已经标记DHCP给了地址
        if (dhcp_supplied_address(netif))    // DHCP已给地址
        {                                   // 分配成功分支开始
          g_lwip_dhcp_state = LWIP_DHCP_ADDRESS_ASSIGNED;  // 进入稳定态，不再重复申请
          if (ip != 0)   // 地址非0才是真拿到了
          {                               // 分支体开始
            g_lwipdev.dhcpstatus = 2;    // 2=DHCP成功

            // 把32位大端地址拆成4个字节存起来，方便打印，右移24拿最高字节
            g_lwipdev.ip[3]      = (uint8_t)(ip >> 24);   // IP最高字节
            g_lwipdev.ip[2]      = (uint8_t)(ip >> 16);   // IP次高字节
            g_lwipdev.ip[1]      = (uint8_t)(ip >> 8);    // IP次低字节
            g_lwipdev.ip[0]      = (uint8_t)(ip);         // IP最低字节
            g_lwipdev.netmask[3] = (uint8_t)(netmask >> 24);  // 掩码最高字节
            g_lwipdev.netmask[2] = (uint8_t)(netmask >> 16);  // 掩码次高字节
            g_lwipdev.netmask[1] = (uint8_t)(netmask >> 8);   // 掩码次低字节
            g_lwipdev.netmask[0] = (uint8_t)(netmask);        // 掩码最低字节
            g_lwipdev.gateway[3] = (uint8_t)(gw >> 24);   // 网关最高字节
            g_lwipdev.gateway[2] = (uint8_t)(gw >> 16);   // 网关次高字节
            g_lwipdev.gateway[1] = (uint8_t)(gw >> 8);    // 网关次低字节
            g_lwipdev.gateway[0] = (uint8_t)(gw);         // 网关最低字节
            printf("DHCP assigned: %d.%d.%d.%d / %d.%d.%d.%d / gw %d.%d.%d.%d\r\n",   // 打印分配结果
                   g_lwipdev.ip[0], g_lwipdev.ip[1], g_lwipdev.ip[2], g_lwipdev.ip[3],   // IP四个字节
                   g_lwipdev.netmask[0], g_lwipdev.netmask[1], g_lwipdev.netmask[2], g_lwipdev.netmask[3],   // 掩码四个字节
                   g_lwipdev.gateway[0], g_lwipdev.gateway[1], g_lwipdev.gateway[2], g_lwipdev.gateway[3]);  // 网关四个字节
          }                                 // 分支体结束
        }                                   // 分配成功分支结束
        else                                // 还没拿到地址
        {                                   // 等待分支开始
          // 还没拿到IP，从netif里拿DHCP客户端状态，看尝试次数超没超限
          dhcp = (struct dhcp *)netif_get_client_data(netif, LWIP_NETIF_CLIENT_DATA_INDEX_DHCP);  // 取DHCP内部状态
          if (dhcp->tries > LWIP_MAX_DHCP_TRIES)   // 尝试次数超限
          {                                 // 超时分支开始
            // DHCP超时，放弃动态分配，回落静态IP
            printf("DHCP timeout, fallback to static IP...\r\n");   // 打印超时回落静态IP
            g_lwip_dhcp_state = LWIP_DHCP_TIMEOUT;  // 进入超时态，不再重复尝试
            netifapi_dhcp_stop(netif);                // 线程安全停掉DHCP
            g_lwipdev.dhcpstatus = 0xFF;             // 0xFF标记现在用静态IP

            // 把静态IP通过netifapi写进网卡，不能直接改内核字段
            {                               // 回落块开始
              ip_addr_t sip, snm, sgw;      // 临时静态三地址
              IP4_ADDR(&sip, g_lwipdev.ip[0], g_lwipdev.ip[1], g_lwipdev.ip[2], g_lwipdev.ip[3]);   // 拼静态IP
              IP4_ADDR(&snm, g_lwipdev.netmask[0], g_lwipdev.netmask[1], g_lwipdev.netmask[2], g_lwipdev.netmask[3]);  // 拼掩码
              IP4_ADDR(&sgw, g_lwipdev.gateway[0], g_lwipdev.gateway[1], g_lwipdev.gateway[2], g_lwipdev.gateway[3]);  // 拼网关
              netifapi_netif_set_addr(netif, &sip, &snm, &sgw);   // 写进网卡
              // 先down再up，强制清ARP缓存、重新初始化，不然ARP表脏了ping不通
              netifapi_netif_set_down(netif);   // 网卡down
              netifapi_netif_set_up(netif);     // 网卡重新up

              // 静态IP没有DHCP给DNS，手动设公共DNS，不然解析不了云平台域名
              {                             // DNS块开始
                ip_addr_t dns1, dns2;      // 两个DNS服务器
                IP4_ADDR(&dns1, 114, 114, 114, 114);   // 114公共DNS
                IP4_ADDR(&dns2, 223, 5, 5, 5);         // 阿里公共DNS
                dns_setserver(0, &dns1);    // 设第一个DNS
                dns_setserver(1, &dns2);    // 设第二个DNS
              }                             // DNS块结束
            }                               // 回落块结束
            printf("Static IP: %d.%d.%d.%d / %d.%d.%d.%d / gw %d.%d.%d.%d\r\n",   // 打印静态IP结果
                   g_lwipdev.ip[0], g_lwipdev.ip[1], g_lwipdev.ip[2], g_lwipdev.ip[3],   // IP四个字节
                   g_lwipdev.netmask[0], g_lwipdev.netmask[1], g_lwipdev.netmask[2], g_lwipdev.netmask[3],   // 掩码四个字节
                   g_lwipdev.gateway[0], g_lwipdev.gateway[1], g_lwipdev.gateway[2], g_lwipdev.gateway[3]);  // 网关四个字节
          }                                 // 超时分支结束
        }                                   // 等待分支结束
      }                                     // case体结束
      break;                                // 跳出switch

      case LWIP_DHCP_LINK_DOWN:             // 断网状态
      {                                       // case体开始
        // 网线断了，停掉DHCP，回到OFF，下次插线回调会置START重新申请
        netifapi_dhcp_stop(netif);          // 停DHCP
        g_lwip_dhcp_state = LWIP_DHCP_OFF;  // 回OFF态
      }                                     // case体结束
      break;                                // 跳出switch

      default:                              // 其他稳定态
        break;   // 成功、超时、OFF这些稳定态，什么都不做
    }                                         // 状态机结束

    vTaskDelay(1000 / portTICK_PERIOD_MS);   // 每秒跑一次状态机，DHCP响应本身就是秒级
  }                                          // 死循环结束
}                                            // DHCP线程结束
#endif // LWIP_DHCP 条件编译结束
