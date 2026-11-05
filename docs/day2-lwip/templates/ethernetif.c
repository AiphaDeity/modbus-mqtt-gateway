/**
 * @file    ethernetif.c
 * @brief   LwIP ↔ ETH 网卡接口（Day2 带 OS 版 · HAL 新架构零拷贝 + LAN8720A + FreeRTOS 信号量）
 *
 * 【这份文件放哪】bsp/ethernet/ 目录下
 *
 * 【和同事工程的分工（重要）】
 *   同事的 Core/Src/eth.c（CubeMX 生成）已做好：
 *     - heth / DMARxDscrTab / DMATxDscrTab / TxConfig 全局变量
 *     - MX_ETH_Init()：HAL_ETH_Init（RMII）+ HAL_ETH_MspInit（GPIO/时钟/中断）
 *     - main.c 里已调用 MX_ETH_Init()
 *   所以本文件：
 *     ✅ 直接 extern 用这些现成对象（不再重复定义）
 *     ❌ 不写 HAL_ETH_MspInit（同事 eth.c 已有，写了会重复定义编译错）
 *     ✅ 只补 LwIP 需要的：零拷贝收发 + 收包线程 + LAN8720 PHY 探测/复位/链接检测
 *
 * 【收发机制】HAL 新架构零拷贝（抄 ST 官方 F429ZI LwIP 例程）
 *   - 接收：DMA 直接写进 lwip pbuf 池（RxAllocate/RxLink 回调），收完中断
 *            HAL_ETH_RxCpltCallback 释放信号量，ethernetif_thread 醒来喂给 tcpip_thread
 *   - 发送：把 pbuf 的 payload 指针直接给 DMA（TxFreeCallback 释放 pbuf）
 *   ★"零拷贝"的意思：以前收一帧要先拷进中间缓冲，再 memcpy 给 lwip；
 *     现在 DMA 直接把网线来的数据写进 lwip 的 pbuf 内存，CPU 全程不用搬数据
 *
 * 【PHY】LAN8720A（抄探索者例程寄存器操作，HAL_ETH_Read/WritePHYRegister 实现）
 *   - PHY 复位：同事 gpio.c 已把 ETH_NRST(PE12) 配为输出高（上电即运行态），
 *     所以这里用"软复位"（BMCR=0x8000）代替硬件脉冲，不依赖 main.c 的调用顺序
 */

/* ================= 一、头文件 ================= */
#include "stm32f4xx_hal.h"     /* ★HAL 主头：HAL_ETH_xxx 全系列 API + ETH_HandleTypeDef 定义 */
#include "lwip/opt.h"          /* lwip 配置头（内含 lwipopts.h），所有 lwip 源文件第一个必须 include 它 */
#include "lwip/def.h"          /* 字节序/宏工具 */
#include "lwip/mem.h"          /* 堆内存 API */
#include "lwip/memp.h"         /* ★内存池 API：LWIP_MEMPOOL_DECLARE/INIT/ALLOC/FREE（零拷贝池的基础） */
#include "lwip/pbuf.h"         /* ★pbuf 结构体 + pbuf_alloced_custom/pbuf_free_custom 机制 */
#include "lwip/timeouts.h"     /* lwip 超时管理 */
#include "lwip/stats.h"        /* 统计（LWIP_STATS=0 时为空实现） */
#include "netif/ethernet.h"    /* ★以太网层：etharp_output / ETH_HWADDR_LEN / ETH_MAX_PAYLOAD 等 */
#include "netif/etharp.h"      /* ARP 输出函数（netif->output 用） */
#include "cmsis_os.h"          /* ★CMSIS-RTOS V2：osSemaphoreNew/Release/Acquire + osThreadNew（同事工程已有） */
#include <string.h>            /* memcpy 用 */
#include <stddef.h>            /* offsetof 用（零拷贝池找结构体头的关键技巧） */

#include "ethernetif.h"        /* 本模块头（对外接口 + 自身声明） */

/* ============================================================================
 * 二、外部对象（来自同事 Core/Src/eth.c，不要重复定义！）
 * ==========================================================================*/
/* ★这些变量在同事 eth.c 里定义，本文件只是"借用"。
   定义和初始化（HAL_ETH_Init + 描述符链表 + TxConfig 校验和配置）都在 main.c 的
   MX_ETH_Init() 里完成了，这里直接用 */
extern ETH_HandleTypeDef heth;                       /* ETH 句柄：所有 HAL_ETH 操作都靠它 */
extern ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT];  /* Rx DMA 描述符数组（4 个，DMA 用它知道往哪写） */
extern ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT];  /* Tx DMA 描述符数组（4 个，DMA 用它知道读哪发） */
extern ETH_TxPacketConfig TxConfig;                  /* Tx 包公共配置（Attributes=CSUM|CRCPAD 已配好） */

/* ============================================================================
 * 三、板级宏定义（天空星·筑基学习板，原理图已确认）
 * ==========================================================================*/
#define LAN8720_PHY_ADDRESS         ((uint8_t)0x00) /* PHY 地址（由 nINTSEL 引脚决定，默认 0） */

/* LAN8720A 寄存器（标准 IEEE 802.3 PHY 寄存器，所有 10/100M PHY 通用）
   ★PHY_BSR / PHY_LINKED_STATUS 同事的 stm32f4xx_hal_conf.h 已定义（值相同），
   这里用 #ifndef 保护避免重复定义警告（有就用自己的，没有才定义） */
#ifndef PHY_BSR
#define PHY_BSR                     ((uint16_t)0x01)   /* 基本状态寄存器：bit2=链接状态 */
#endif
#define PHY_IDR1                    ((uint16_t)0x02)   /* PHY 标识符寄存器 1（厂商号高 16 位） */
#define PHY_IDR2                    ((uint16_t)0x03)   /* PHY 标识符寄存器 2（厂商号低 16 位） */
#define PHY_BMCR                    ((uint16_t)0x00)   /* 基本控制寄存器：软复位/自动协商开关 */

/* LAN8720A 的识别 ID（数据手册给的值，用来确认 MDIO 对面确实是 LAN8720） */
#define LAN8720_IDR1_VAL            ((uint16_t)0x0007) /* IDR1 值：LAN8720A 高 16 位 */
#define LAN8720_IDR2_VAL            ((uint16_t)0xC0F1) /* IDR2 值：LAN8720A 低 16 位 */

#ifndef PHY_LINKED_STATUS
#define PHY_LINKED_STATUS           ((uint16_t)0x0004) /* BSR bit2=1：链路已建立（网线插好且对端活着） */
#endif
#define BMCR_SOFT_RESET             ((uint16_t)0x8000) /* BMCR bit15=1：软复位（写 1 触发，完成后硬件自动清零） */
#define BMCR_AUTONEGO_EN            ((uint16_t)0x1000) /* BMCR bit12=1：使能自动协商（自动定速率/双工） */

/* 网卡名（lwip 的 netif->name，调试用） */
#define IFNAME0 's'
#define IFNAME1 't'

/* 零拷贝 Rx 池的缓冲个数：必须 ≥ DMA Rx 描述符数（ETH_RX_DESC_CNT=4），
   否则 DMA 收满一轮后没有新缓冲可填。给 10 个留余量 */
#define ETH_RX_BUFFER_CNT           10

/* ============================================================================
 * 四、零拷贝 Rx 池（核心数据结构）
 * ==========================================================================*/
/* ★每个接收缓冲的结构：
   - pbuf_custom：lwip 的"自定义 pbuf"头（里面存自定义释放函数指针）
   - buff[]：真正的数据区（32 字节对齐，DMA 直接往里写）
   buff 的地址就是 DMA 描述符要填的地址；结构体头（pbuf_custom）用 offsetof 反推 */
typedef struct
{
  struct pbuf_custom pbuf_custom;                            /* lwip 自定义 pbuf 头 */
  uint8_t buff[(ETH_RX_BUF_SIZE + 31) & ~31] __ALIGNED(32);  /* 32 字节对齐数据区 */
} RxBuff_t;

/* ★声明一个 lwip 内存池 RX_POOL：元素 = RxBuff_t，个数 = ETH_RX_BUFFER_CNT。
   这是"零拷贝"的关键：这块内存既是 DMA 的目标，也是 lwip pbuf 的载体，
   DMA 写完它，lwip 直接拿着它当 pbuf 用，不用任何拷贝 */
LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");

/* 收包信号量（二值）：ETH 接收中断里 release（给一下），ethernetif_thread 里 acquire（取一下）。
   ★为什么用信号量不用消息队列？收包只要"有货了"这一个通知，不需要传数据本身
   （数据已经在 pbuf 里了），信号量最轻量。
   ★为什么二值不是计数？DMA 一次中断对应"至少有一帧"，线程醒来会把积压帧全读完 */
static osSemaphoreId_t RxPktSemaphore;

/* 收包线程属性：名字 "ethif"、栈 512 字（≈2KB）、优先级 High。
   栈大小参考 ST 官方例程原值；优先级 High 够用（比 DHCP/链接线程高，
   保证收包不被业务线程饿着，但比 tcpip 内核线程低） */
static const osThreadAttr_t ethernetif_thread_attr = {
  .name       = "ethif",
  .stack_size = 512,
  .priority   = osPriorityHigh,
};

/* ============================================================================
 * 五、函数声明
 * ==========================================================================*/
static void low_level_init(struct netif *netif);            /* 网卡底层初始化（ethernetif_init 内部调） */
static err_t  low_level_output(struct netif *netif, struct pbuf *p);  /* 发一帧 */
static struct pbuf *low_level_input(struct netif *netif);   /* 取一帧（从 DMA 队列） */
static void ethernetif_thread(void *argument);              /* 收包线程：等信号量 → 读帧 → 喂内核 */
void pbuf_free_custom(struct pbuf *p);                      /* 自定义 pbuf 释放（RxAllocate 注册的） */

/* LAN8720A PHY 内部函数 */
static uint8_t lan8720_read_reg(uint16_t reg, uint32_t *val);   /* 读 PHY 寄存器（HAL 封装） */
static uint8_t lan8720_write_reg(uint16_t reg, uint32_t val);   /* 写 PHY 寄存器（HAL 封装） */

/* ============================================================================
 * 六、PHY：LAN8720A
 * ==========================================================================*/

/** 读 PHY 寄存器。
    MDIO 接口由 HAL 管理（HAL_ETH_ReadPHYRegister 内部操作 MDIO 时序），
    ★注意：MDIO 读在 HAL_ETH_Init 之后才能用（MDIO 时钟等已配好） */
static uint8_t lan8720_read_reg(uint16_t reg, uint32_t *val)
{
  if (HAL_ETH_ReadPHYRegister(&heth, LAN8720_PHY_ADDRESS, reg, val) != HAL_OK)
    return 1;      /* 读失败（返回 1 给上层判断） */
  return 0;
}

/** 写 PHY 寄存器 */
static uint8_t lan8720_write_reg(uint16_t reg, uint32_t val)
{
  if (HAL_ETH_WritePHYRegister(&heth, LAN8720_PHY_ADDRESS, reg, val) != HAL_OK)
    return 1;
  return 0;
}

/**
 * @brief  读 PHY 链接状态（ethernet.c 链接线程每秒轮询用）
 * @retval 1=已链接（网线插好且对端活着），0=未链接
 */
uint8_t lan8720_link_read(void)
{
  uint32_t val = 0;
  if (lan8720_read_reg(PHY_BSR, &val) != 0)
    return 0;                     /* 读失败按"未链接"处理 */
  return (val & PHY_LINKED_STATUS) ? 1 : 0;   /* BSR 的 bit2=1 就是链接建立 */
}

/**
 * @brief  LAN8720A 探测 + 初始化：读 ID 验证 → 软复位 → 自动协商
 * @retval 0=成功，1=失败（MDIO 不通 / PHY 不是 LAN8720 / 复位超时）
 */
static uint8_t lan8720_init(void)
{
  uint32_t idr1 = 0, idr2 = 0, val = 0;
  uint16_t timeout = 0;

  /* 1. 读 PHY ID 验证是 LAN8720A
     ★这一步同时验证了 MDIO 通路：如果 MDIO 线没接好/PHY 没上电，
     读出来不会是 0x0007 / 0xC0F1，直接判定失败 */
  if (lan8720_read_reg(PHY_IDR1, &idr1) != 0) return 1;
  if (lan8720_read_reg(PHY_IDR2, &idr2) != 0) return 1;
  if ((idr1 != LAN8720_IDR1_VAL) || (idr2 != LAN8720_IDR2_VAL)) return 1;

  /* 2. 软复位（BMCR bit15=1）
     为什么不用硬件复位脉冲？同事 gpio.c 已把 PE12 配为输出高（PHY 上电即运行），
     再拉低拉高会受 main.c 调用顺序限制；软复位走 MDIO，任何时候都能做 */
  if (lan8720_write_reg(PHY_BMCR, BMCR_SOFT_RESET) != 0) return 1;
  /* 等复位完成：硬件完成软复位后会自动把 bit15 清零。
     加超时保护（最多 1000 次循环），防止死等 */
  do {
    if (lan8720_read_reg(PHY_BMCR, &val) != 0) return 1;
    timeout++;
  } while ((val & BMCR_SOFT_RESET) && (timeout < 1000));
  if (val & BMCR_SOFT_RESET) return 1;   /* 超时了还没复位完 = 失败 */

  /* 3. 自动协商（BMCR bit12=1 + 100M 能力位 0x0100）
     让 PHY 自动和交换机/电脑协商速率（10/100M）和双工模式。
     0x0100 = 100M 全双工能力位，LAN8720 支持，协商时双方取共同最高能力 */
  if (lan8720_write_reg(PHY_BMCR, BMCR_AUTONEGO_EN | 0x0100) != 0) return 1;

  return 0;   /* 全部成功 */
}

/* ============================================================================
 * 七、MAC 地址：用 STM32 唯一 ID（UID）生成（抄探索者 GET_STM32MACID）
 *
 * 为什么不用同事 eth.c 的固定 MAC（00:80:E1:00:00:00）？
 *   固定 MAC 没问题，但多块板子接同一网段时 MAC 全一样，交换机会乱。
 *   用 UID（每颗芯片唯一）生成，保证每块板 MAC 不同。
 * 生成后要同时写两处，保证一致：
 *   - netif->hwaddr（lwip 侧：ARP/DHCP 报文里带的源 MAC）
 *   - HAL 寄存器（HAL 侧：DMA 实际发出的帧的源 MAC）
 *   两处不一致 = ARP 报一个地址、实际发帧是另一个，直接断网
 * ==========================================================================*/
static void get_stm32_mac(uint8_t *mac)
{
  uint32_t uid[3];    /* UID 96 位 = 3 个 32 位寄存器 */
  uint32_t sum = 0, xor = 0;
  uint8_t  buf[12];
  uint8_t  i;

  /* STM32F4 的 UID 基地址 0x1FFF7A10（数据手册），3 个寄存器各 32 位 */
  uid[0] = *(uint32_t *)(0x1FFF7A10);
  uid[1] = *(uint32_t *)(0x1FFF7A14);
  uid[2] = *(uint32_t *)(0x1FFF7A18);

  /* 把 12 个字节拆出来，用来算校验字节 */
  for (i = 0; i < 4; i++) { buf[i]   = (uint8_t)(uid[0] >> (8 * i)); }
  for (i = 0; i < 4; i++) { buf[4+i] = (uint8_t)(uid[1] >> (8 * i)); }
  for (i = 0; i < 4; i++) { buf[8+i] = (uint8_t)(uid[2] >> (8 * i)); }

  /* MAC 后 2 字节用"校验和"和"异或和"生成，保证不同芯片大概率不同 */
  for (i = 0; i < 12; i++) { sum += buf[i]; }   /* 所有字节累加 */
  for (i = 0; i < 12; i++) { xor ^= buf[i]; }   /* 所有字节异或 */

  /* 前 4 字节直接用 UID[0] 的 4 个字节 */
  mac[0] = (uint8_t)((uid[0] & 0xFF000000) >> 24);
  mac[1] = (uint8_t)((uid[0] & 0x00FF0000) >> 16);
  mac[2] = (uint8_t)((uid[0] & 0x0000FF00) >> 8);
  mac[3] = (uint8_t)(uid[0] & 0x000000FF);
  mac[4] = (uint8_t)(sum & 0xFF);   /* 校验和 */
  mac[5] = (uint8_t)(xor & 0xFF);   /* 异或和 */
}

/* ============================================================================
 * 八、LwIP 底层接口（HAL 新架构零拷贝）
 * ==========================================================================*/

/**
 * @brief  硬件侧准备（netif_add → ethernetif_init → 本函数）
 * @note   HAL_ETH_Init 已在 main.c 的 MX_ETH_Init() 完成，这里只做：
 *         netif 参数 + MAC 同步 + Rx 池 + PHY + Start_IT + 信号量 + 收包线程
 */
static void low_level_init(struct netif *netif)
{
  uint8_t macaddress[6] = {0};

  /* 1. netif 基本参数 */
  netif->mtu = ETH_MAX_PAYLOAD;                  /* MTU=1500（以太网最大负载） */
  netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
  /* NETIF_FLAG_BROADCAST：支持广播（DHCP/ARP 都要广播）
     NETIF_FLAG_ETHARP：支持 ARP（同网段找 IP 必须）
     ★注意：这里不置 NETIF_FLAG_LINK_UP！链接状态由链接线程/回调管理，
     上电先 down，等 PHY 探测 + 链接检测通过再 up */

  /* 2. MAC：UID 生成 → 写 lwip 侧 + 同步 HAL 寄存器（覆盖 eth.c 的固定 MAC） */
  get_stm32_mac(macaddress);
  netif->hwaddr_len = ETH_HWADDR_LEN;            /* 6 字节 */
  memcpy(netif->hwaddr, macaddress, 6);          /* lwip 侧 MAC */

  /* ★HAL 侧 MAC：F4 HAL 1.28.3 没有 HAL_ETH_SetMACAddress() API（那是 F7/H7 系列专属），
     F407 的写法是直接操作 MAC 地址寄存器 MACA0LR / MACA0HR——
     这正是 HAL_ETH_Init 内部做的事（把 heth.Init.MACAddr 指向的 6 字节写进这两个寄存器）。
     这里再写一遍，覆盖同事 eth.c 的固定 MAC 00:80:E1:00:00:00，
     保证 ARP 报文里的源 MAC 与实际发帧的源 MAC 完全一致（两处不一致会直接断网）。
     注意：必须在 HAL_ETH_Init（MX_ETH_Init）之后执行才有效（ETH 时钟已使能）。
     MACA0LR = mac[0..3]（低 4 字节，mac[0] 在最低字节）
     MACA0HR = mac[4..5]（高 2 字节）+ bit31 SA（源地址有效位，必须置 1） */
  ETH->MACA0LR = (uint32_t)((uint32_t)macaddress[0] |
                ((uint32_t)macaddress[1] << 8) |
                ((uint32_t)macaddress[2] << 16) |
                ((uint32_t)macaddress[3] << 24));
  ETH->MACA0HR = (uint32_t)((uint32_t)macaddress[4] |
                ((uint32_t)macaddress[5] << 8) | 0x80000000U);

  /* 3. 零拷贝接收池：把 RX_POOL 初始化（分配内存）。必须在 Start_IT 之前，
     因为 DMA 启动后立刻要 RxAllocate 回调从池里取缓冲 */
  LWIP_MEMPOOL_INIT(RX_POOL);

  /* 4. PHY：探测 + 软复位 + 自动协商。
     失败（MDIO 不通/不是 LAN8720）就标记网卡 down，不启动 ETH */
  if (lan8720_init() != 0)
  {
    netif_set_link_down(netif);
    netif_set_down(netif);
    return;
  }

  /* 5. 启动 ETH（★关键动作）
     HAL_ETH_Start_IT 做的事：使能 MAC + DMA，注册接收中断，
     之后 DMA 就开始往 RX_POOL 写数据，每写完一帧调 HAL_ETH_RxCpltCallback */
  HAL_ETH_Start_IT(&heth);

  /* 6. 收包信号量（二值，初始 0）
     osSemaphoreNew(1, 0, NULL)：1=最大计数（二值）、0=初始没货、NULL=默认属性 */
  RxPktSemaphore = osSemaphoreNew(1, 0, NULL);

  /* 7. 初始链接状态上报：上电先把 PHY 链接状态同步给 lwip */
  if (lan8720_link_read())
  {
    netif_set_link_up(netif);   /* 链接建立（网线已插好） */
    netif_set_up(netif);        /* 网口可用（lwip 开始对外服务） */
  }
  else
  {
    netif_set_link_down(netif);
    netif_set_down(netif);      /* 没插线：先 down，链接线程发现插线后会拉起 */
  }

  /* 8. 创建收包线程：等信号量 → 读帧 → 喂给 tcpip_thread
     ★为什么在这里创建？因为信号量刚建好，且只有 PHY 探测成功后收包线程才有意义 */
  osThreadNew(ethernetif_thread, netif, &ethernetif_thread_attr);
}

/**
 * @brief  发送一帧（netif->linkoutput = low_level_output）
 * @note   零拷贝：pbuf payload 指针直接给 DMA；发完由 TxFreeCallback 释放
 * @retval ERR_OK 成功 / ERR_IF 失败
 */
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  uint32_t i = 0U;                     /* 当前 pbuf 链第几段 */
  struct pbuf *q = NULL;               /* 遍历 pbuf 链的游标 */
  ETH_BufferTypeDef Txbuffer[ETH_TX_DESC_CNT] = {0};  /* DMA 要发的"分段表"（最多 4 段） */

  /* pbuf 可能是一整块，也可能是一串（链表）。把每段的 payload 指针
     填进 Txbuffer 数组，DMA 按这个表把各段数据串成一帧发出去 */
  for (q = p; q != NULL; q = q->next)
  {
    if (i >= ETH_TX_DESC_CNT)          /* 段数超过描述符数（4），发不了 */
      return ERR_IF;

    Txbuffer[i].buffer = q->payload;   /* ★只给指针，不拷数据（零拷贝） */
    Txbuffer[i].len    = q->len;

    /* 把分段表串成链表：每段的 next 指下一段，最后一段 next=NULL */
    if (i > 0)
      Txbuffer[i-1].next = &Txbuffer[i];
    if (q->next == NULL)
      Txbuffer[i].next = NULL;

    i++;
  }

  /* 填 TxConfig（公共部分同事已配好：校验和/CRC 硬件算，这里只填每帧特有的） */
  TxConfig.Length   = p->tot_len;      /* 整帧总长（DMA 发这么多字节） */
  TxConfig.TxBuffer = Txbuffer;        /* 分段表 */
  TxConfig.pData    = p;               /* ★把 pbuf 存进 pData，发完回调能取回它来释放 */

  pbuf_ref(p);                         /* ★引用计数 +1：
     调用者（lwip）发完这帧后可能立刻 free 这个 pbuf，
     但 DMA 还在读它的内存！引用 +1 保证 DMA 发完前 pbuf 不会被真正释放 */

  if (HAL_ETH_Transmit_IT(&heth, &TxConfig) != HAL_OK)
  {
    pbuf_free(p);                      /* 发送失败：把刚才 +1 的引用还回去 */
    return ERR_IF;
  }

  return ERR_OK;   /* 发出去，DMA 完成后会调 HAL_ETH_TxFreeCallback 释放 pbuf */
}

/**
 * @brief  取一帧接收数据（零拷贝）
 * @note   HAL_ETH_ReadData 从 DMA 收队列取一帧，返回的 p 就是
 *         HAL_ETH_RxAllocateCallback 里 alloc 的那个 pbuf（零拷贝，没搬过数据）
 */
static struct pbuf *low_level_input(struct netif *netif)
{
  struct pbuf *p = NULL;
  HAL_ETH_ReadData(&heth, (void **)&p);   /* 取 pbuf 指针（内部会更新 DMA 描述符） */
  return p;
}

/**
 * @brief  收包线程：等信号量 → 读帧 → 交给 tcpip_thread（netif->input）
 * @note   这就是"带 OS 版"的收包姿势：
 *         裸跑版是主循环里手动轮询；这里线程挂起在信号量上，
 *         接收中断一来 osSemaphoreRelease 唤醒它，不空转不丢包
 */
static void ethernetif_thread(void *argument)
{
  struct netif *netif = (struct netif *)argument;
  struct pbuf *p = NULL;

  for (;;)
  {
    /* 挂起等待：没有包时线程睡觉，不占 CPU（★RTOS 的意义所在） */
    osSemaphoreAcquire(RxPktSemaphore, osWaitForever);

    /* 醒来后把积压帧全部读完（中断可能一次性来了多帧，信号量只是"有货"通知）
       每帧都通过 netif->input(=tcpip_input) 扔给内核线程处理 */
    while ((p = low_level_input(netif)) != NULL)
    {
      if (netif->input(p, netif) != ERR_OK)   /* 内核处理失败（内存不够等） */
        pbuf_free(p);                          /* 自己释放，不能丢 */
    }
  }
}

/**
 * @brief  网卡初始化入口（netif_add 的回调，ethernet.c 里注册）
 *         配置 netif 的名字/收发函数，然后调 low_level_init 做硬件准备
 */
err_t ethernetif_init(struct netif *netif)
{
  LWIP_ASSERT("netif != NULL", (netif != NULL));   /* 断言：参数不能空 */

#if LWIP_NETIF_HOSTNAME
  netif->hostname = "m2-gateway";                  /* 可选：给网卡起个名（DHCP 上报） */
#endif

  netif->name[0] = IFNAME0;                        /* 网卡名 "st"（调试打印用） */
  netif->name[1] = IFNAME1;

  netif->output     = etharp_output;               /* ★发包：先查 ARP 再走 linkoutput（标准以太网流程） */
  netif->linkoutput = low_level_output;            /* ★真正发帧：就是上面的零拷贝发送 */

  low_level_init(netif);                           /* 硬件准备（PHY + Start_IT + 线程） */

  return ERR_OK;                                   /* 初始化成功 */
}

/* ============================================================================
 * 九、HAL_ETH 回调族（零拷贝核心 + 中断）
 *
 * ★这 6 个函数是 HAL 库里的"弱符号"（__weak），本文件定义了就覆盖掉，
 *   由 HAL_ETH_Start_IT 之后的中断自动调用，不需要注册。
 * ==========================================================================*/

/** 自定义 pbuf 释放：归还 RX_POOL。
    这个函数指针被存在 pbuf_custom.custom_free_function 里，
    lwip 释放这个 pbuf 时会自动调它（而不是普通 pbuf 的释放） */
void pbuf_free_custom(struct pbuf *p)
{
  struct pbuf_custom *custom_pbuf = (struct pbuf_custom *)p;
  LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);   /* ★还回池子，下次接收还能用 */
}

/** Rx 缓冲分配回调：从 RX_POOL 取一块给 DMA 描述符
    ★零拷贝第一环：DMA 启动前/运行中缺缓冲时，HAL 调这个函数拿一块内存，
    把地址给 DMA 描述符（*buff），DMA 直接往这块内存写网线来的数据 */
void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
  struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
  if (p != NULL)
  {
    /* ★关键技巧（offsetof）：
       buff 指向的是 RxBuff_t 里的数据区，但释放时需要 pbuf_custom 的地址，
       所以用 "数据区地址 - 结构体内偏移" 反推回结构体头。
       这就是零拷贝能"一个结构体两用"的秘诀 */
    *buff = (uint8_t *)p + offsetof(RxBuff_t, buff);
    p->custom_free_function = pbuf_free_custom;     /* 记下自定义释放函数 */
    /* 用这块内存构造一个 lwip pbuf（PBUF_REF：引用外部内存，不拷数据） */
    pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUF_SIZE);
  }
  else
  {
    *buff = NULL;   /* 池空了（理论不会，给了 10 个缓冲） */
  }
}

/** Rx 链回调：把 DMA 分段写入的帧串成 pbuf 链
    一帧数据可能被 DMA 分到多个缓冲里（巨帧/内存碎片），
    这个回调把各段按顺序串成 pbuf 链表，并累加 tot_len */
void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
  struct pbuf **ppStart = (struct pbuf **)pStart;
  struct pbuf **ppEnd   = (struct pbuf **)pEnd;
  struct pbuf *p = NULL;

  /* 反推回 pbuf 头（和 RxAllocate 同一个 offsetof 技巧） */
  p = (struct pbuf *)(buff - offsetof(RxBuff_t, buff));
  p->next = NULL;
  p->tot_len = 0;      /* 先清零，下面累加 */
  p->len = Length;     /* 本段长度 */

  /* 串链：第一段就是链头，后面每段接在链尾 */
  if (*ppStart == NULL)
    *ppStart = p;
  else
    (*ppEnd)->next = p;
  *ppEnd = p;

  /* 累加整帧总长（lwip 协议栈按 tot_len 知道一帧多大） */
  for (p = *ppStart; p != NULL; p = p->next)
  {
    p->tot_len += Length;
  }
}

/** Tx 完成释放回调：释放 pbuf（零拷贝发送的归还）
    buff 参数 = low_level_output 里存进 TxConfig.pData 的那个 pbuf */
void HAL_ETH_TxFreeCallback(uint32_t *buff)
{
  pbuf_free((struct pbuf *)buff);   /* ★DMA 发完了，pbuf 可以真正释放了 */
}

/** 接收中断回调：释放收包信号量，唤醒 ethernetif_thread
    ★这是"中断 → 线程"的经典桥接：中断里只做最轻的事（release 信号量），
    重活（协议栈处理）留给被唤醒的线程，绝不占中断时间 */
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)
{
  (void)heth;
  osSemaphoreRelease(RxPktSemaphore);   /* 给信号量（从中断调，CMSIS-RTOS 支持） */
}

/** 发送完成回调（释放由 TxFreeCallback 处理，这里留空） */
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth)
{
  (void)heth;
}

/** ETH 错误回调（DMA 错误等；暂不处理，可后续加错误恢复） */
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth)
{
  (void)heth;
}
