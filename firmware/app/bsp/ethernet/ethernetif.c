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
#include <stdio.h>          /* ★printf（排障打印用，重定向已在 main.c 实现） */
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
#define PHY_LINKED_STATUS           ((uint16_t)0x0004) /* BSR bit2=1：链路已建立（IEEE 802.3 BMSR bit2 Link Status） */
#endif
#define BMCR_SOFT_RESET             ((uint16_t)0x8000) /* BMCR bit15=1：软复位（写 1 触发，完成后硬件自动清零） */
#define BMCR_AUTONEGO_EN            ((uint16_t)0x1000) /* BMCR bit12=1：使能自动协商（自动定速率/双工） */

/* 网卡名（lwip 的 netif->name，调试用） */
#define IFNAME0 's'
#define IFNAME1 't'

/* 零拷贝 Rx 池的缓冲个数：必须 ≥ DMA Rx 描述符数（ETH_RX_DESC_CNT=4）。
   ★★★2026-11-10 修复 RBUS 死锁：原 6 个（4 DMA + 2 余量）太少，
   TCP/DHCP/DNS 流量涌入时 lwip 暂持 pbuf → 池耗尽 → RxAllocate 返回 NULL
   → DMA 描述符得不到新缓冲 → RBUS（0x8080）→ DMA RX 挂起 → CONNACK 永久丢失。
   改为 16：4 DMA + 12 余量，即使 lwip 暂持 8+ 个 pbuf 也不会饿死 DMA。
   RAM 代价：+10×~1584B ≈ +15.5KB（RAM 67%→~79%，128KB 内可接受） */
#define ETH_RX_BUFFER_CNT           16

/* ★以太网最小帧长补齐（60 字节 = 14 帧头 + 46 负载，FCS 由 MAC 另加 4 字节 → 线上 64 字节）
   lwIP 发的 ARP 应答/请求只有 42 字节（14 头 + 28 ARP），短于以太网最小帧长 64。
   ★说明（实测结论，别再走弯路）：本次 PING 排障过程中曾把「ARP 帧只有 42 字节被交换机/网卡
     当 runt 丢弃」当成病因，专门做了补齐实验 —— 补到 60 字节后 PC 依然学不到板子 MAC，
     最终查明真因是 Tx 描述符没回收（见 low_level_output 里 HAL_ETH_ReleaseTxPacket 的注释）。
     STM32F4 的 MAC 本身带 CRCPAD 功能（eth.c 里 TxConfig 已配 ETH_TX_PACKETS_FEATURES_CRCPAD），
     会自动补 pad + 加 FCS，所以这里的补齐属于"规范层面的双保险"，不是必须的。
   做法：不足 60 字节的帧在软件里拷进一个静态缓冲、尾部补 0，再交给 DMA 发。
   缓冲用 ETH_TX_DESC_CNT 份轮转，避免上一个帧还没被 DMA 读完就被复用（发送是异步的） */
#define ETH_MIN_FRAME_LEN           60U

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
static osThreadId_t RxThread;
static uint8_t RxPoolInitialized;

/* ★帧级排障打印开关（临时）：1=打印收发帧摘要（上限各 120 帧）+ [CNT] 心跳，0=关闭。
   排查"收了不发/发了不到"这类链路问题时打开，问题定位后改回 0。
   ★PING 已通（2026-11-04），这里按 0 关闭；以后查链路再改 1 即可，不占串口带宽 */
#define ETH_DBG_FRAMES   0   /* 排查链路问题时改 1，定位后改回 0 */

/* ★★排障计数器（诊断用，量很小，长期保留也不碍事）：
   gdbg_rx_irq   —— 收包中断次数（DMA 每收到一帧 +1）
   gdbg_err_irq  —— ETH 错误中断次数（DMA 报错时 +1，会把 HAL 状态打成 ERROR）
   gdbg_rd_fail  —— HAL_ETH_ReadData 返回失败次数（读不出帧）
   gdbg_err_code —— 最近一次 DMAErrorCode（定位是哪种 DMA 错）
   gdbg_pool_alloc —— RX_POOL 分配次数（RxAllocate 被调）
   gdbg_pool_null  —— RX_POOL 分配失败次数（池空，返回 NULL → 是 RBUS 根因）
   gdbg_pool_free  —— RX_POOL 释放次数（pbuf_free_custom 被调）
   gdbg_rbus_recover —— RBUS 恢复尝试次数（手动 kick DMARPDR） */
volatile uint32_t gdbg_rx_irq   = 0U;
volatile uint32_t gdbg_err_irq  = 0U;
volatile uint32_t gdbg_rd_fail  = 0U;
volatile uint32_t gdbg_err_code = 0U;
volatile uint32_t gdbg_pool_alloc = 0U;
volatile uint32_t gdbg_pool_null  = 0U;
volatile uint32_t gdbg_pool_free  = 0U;
volatile uint32_t gdbg_rbus_recover = 0U;

/* 收包线程属性：名字 "ethif"、栈 1024 字节、优先级 High。
   ★栈为什么从例程的 512 加到 1024：本线程里要调 printf（调试打印）和
   tcpip_input，512 字节（=128 个字）放不下 picolibc printf 的临时开销，
   一旦溢出会直接触发 vApplicationStackOverflowHook 卡死在 Error_Handler */
static const osThreadAttr_t ethernetif_thread_attr = {
  .name       = "ethif",
  .stack_size = 1024,
  .priority   = osPriorityHigh,
};

/* ============================================================================
 * 五、函数声明
 * ==========================================================================*/
static err_t low_level_init(struct netif *netif);           /* 网卡底层初始化（ethernetif_init 内部调） */
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
  static uint8_t last_fail = 0;   /* ★排障：只在"失败↔成功"转变时打印一次，避免每秒刷屏 */
  if (lan8720_read_reg(PHY_BSR, &val) != 0)
  {
    if (!last_fail) { printf("[PHY] link read FAIL (MDIO no response)\r\n"); last_fail = 1; }
    return 0;                     /* 读失败按"未链接"处理 */
  }
  if (last_fail) { printf("[PHY] link read OK again (BSR=0x%04X)\r\n", (uint16_t)val); last_fail = 0; }
  uint8_t cur = (val & PHY_LINKED_STATUS) ? 1 : 0;   /* BSR bit2=1 就是链接建立 */
  /* ★调试：link 状态变化时打印 BSR 完整值（验证 mask 修复） */
  static uint8_t last_link = 0xFF;
  if (cur != last_link)
  {
    printf("[PHY] link %s (BSR=0x%04X bit2=%d)\r\n", cur ? "UP" : "DOWN", (uint16_t)val, (val & 0x0004) ? 1 : 0);
    last_link = cur;
  }
  return cur;
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
  if (lan8720_read_reg(PHY_IDR1, &idr1) != 0) { printf("[PHY] read IDR1 FAIL (MDIO no response?)\r\n"); return 1; }
  if (lan8720_read_reg(PHY_IDR2, &idr2) != 0) { printf("[PHY] read IDR2 FAIL (MDIO no response?)\r\n"); return 1; }
  /* ★排障打印：把实际读到的 ID 打出来。0x0007/0xC0F1=PHY 活着；
     0xFFFF=MDIO 没接上/PHY 没上电/时钟没起振；其他值=PHY 地址不对 */
  printf("[PHY] IDR1=0x%04X IDR2=0x%04X (expect 0x0007/0xC0F1)\r\n",
         (uint16_t)idr1, (uint16_t)idr2);
  if ((idr1 != LAN8720_IDR1_VAL) || (idr2 != LAN8720_IDR2_VAL))
  {
    printf("[PHY] ID MISMATCH -> PHY not responding (check power/clock/reset)\r\n");
    return 1;
  }
  printf("[PHY] LAN8720A detected, MDIO OK\r\n");

  /* 2. 软复位（BMCR bit15=1）
     为什么不用硬件复位脉冲？同事 gpio.c 已把 PE12 配为输出高（PHY 上电即运行），
     再拉低拉高会受 main.c 调用顺序限制；软复位走 MDIO，任何时候都能做 */
  if (lan8720_write_reg(PHY_BMCR, BMCR_SOFT_RESET) != 0) { printf("[PHY] soft-reset write FAIL\r\n"); return 1; }
  /* 等复位完成：硬件完成软复位后会自动把 bit15 清零。
     加超时保护（最多 1000 次循环），防止死等 */
  do {
    if (lan8720_read_reg(PHY_BMCR, &val) != 0) { printf("[PHY] soft-reset poll read FAIL\r\n"); return 1; }
    timeout++;
  } while ((val & BMCR_SOFT_RESET) && (timeout < 1000));
  if (val & BMCR_SOFT_RESET) { printf("[PHY] soft-reset TIMEOUT\r\n"); return 1; }
  printf("[PHY] soft-reset done\r\n");

  /* 3. 自动协商（BMCR bit12=1 + 100M 能力位 0x0100）
     让 PHY 自动和交换机/电脑协商速率（10/100M）和双工模式。
     0x0100 = 100M 全双工能力位，LAN8720 支持，协商时双方取共同最高能力 */
  if (lan8720_write_reg(PHY_BMCR, BMCR_AUTONEGO_EN | 0x0100) != 0) { printf("[PHY] autonego write FAIL\r\n"); return 1; }
  printf("[PHY] autonego started\r\n");

  /* 3b. 等自协商完成（BSR bit5=1），最多等 3 秒。
   * ★某些交换机/路由器不响应 FLP 自协商脉冲，LAN8720 会永远等下去，
   *   表现为灯亮但 BSR bit2(link)=0 bit5(autonego done)=0。超时后强制 100M 全双工。 */
  timeout = 0;
  do {
    osDelay(10);
    if (lan8720_read_reg(PHY_BSR, &val) != 0) { printf("[PHY] autonego poll FAIL\r\n"); break; }
    timeout++;
  } while (((val & 0x0020) == 0) && (timeout < 300));   /* bit5=Auto-Neg Complete */
  if (val & 0x0020)
  {
    printf("[PHY] autonego done (BSR=0x%04X)\r\n", (uint16_t)val);
  }
  else
  {
    printf("[PHY] autonego TIMEOUT (BSR=0x%04X), forcing 100M FD\r\n", (uint16_t)val);
    /* 强制 100M 全双工：BMCR bit13=1(100M) bit8=1(FD) bit12=0(禁自协商) = 0x2100 */
    lan8720_write_reg(PHY_BMCR, 0x2100);
    osDelay(100);
    lan8720_read_reg(PHY_BSR, &val);
    printf("[PHY] after force: BSR=0x%04X link=%d\r\n", (uint16_t)val, (val & 0x0004) ? 1 : 0);
  }

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

  /* IEEE 802 MAC 首字节规则：bit0=I/G（0=单播，1=组播）；bit1=U/L（0=全局管理OUI，1=本地管理）。
     UID 截出来的前 3 字节不对应任何注册 OUI，绝不能当全局地址用——必须置成本地管理位，
     同时清 bit0 保证是单播地址。实测板子 MAC 由此从 00:2D:.. 变为 02:2D:.. */
  mac[0] = (uint8_t)((mac[0] & 0xFCU) | 0x02U);
}

/* ============================================================================
 * 八、LwIP 底层接口（HAL 新架构零拷贝）
 * ==========================================================================*/

/* 仅在 DMA 尚未启动时回收接收资源；删除失败保留句柄供下次重试。 */
static err_t ethernetif_release_rx(void)
{
  if (RxThread != NULL)
  {
    if (osThreadTerminate(RxThread) != osOK)
      return ERR_IF;
    RxThread = NULL;
  }
  if (RxPktSemaphore != NULL)
  {
    if (osSemaphoreDelete(RxPktSemaphore) != osOK)
      return ERR_IF;
    RxPktSemaphore = NULL;
  }
  return ERR_OK;
}

/* 初始化接收路径；返回 ERR_OK 才允许将网卡加入协议栈。 */
static err_t low_level_init(struct netif *netif)
{
  uint8_t macaddress[6] = {0};

  if (heth.gState != HAL_ETH_STATE_READY || ethernetif_release_rx() != ERR_OK)
    return ERR_IF;

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
     MACA0HR = mac[4..5]（高 2 字节）；bit31 = AE（Address Enable，本表项有效），
     F4 参考手册要求写 1，这不是"源地址有效位"的通用规则，只针对本寄存器。 */
  ETH->MACA0LR = (uint32_t)((uint32_t)macaddress[0] |
                ((uint32_t)macaddress[1] << 8) |
                ((uint32_t)macaddress[2] << 16) |
                ((uint32_t)macaddress[3] << 24));
  ETH->MACA0HR = (uint32_t)((uint32_t)macaddress[4] |
                ((uint32_t)macaddress[5] << 8) | 0x80000000U);   /* bit31=AE，必须置 1 */

  /* 3. 零拷贝接收池：把 RX_POOL 初始化（分配内存）。必须在 Start_IT 之前，
     因为 DMA 启动后立刻要 RxAllocate 回调从池里取缓冲 */
  if (!RxPoolInitialized)
  {
    LWIP_MEMPOOL_INIT(RX_POOL);
    RxPoolInitialized = 1;
  }

  /* 4. PHY：探测 + 软复位 + 自动协商。
     失败（MDIO 不通/不是 LAN8720）就标记网卡 down，不启动 ETH */
  if (lan8720_init() != 0)
  {
    netif_set_link_down(netif);
    netif_set_down(netif);
    return ERR_IF;
  }

  /* 5. 收包信号量（二值，初始 0）——★必须先建信号量再启动 ETH！
     osSemaphoreNew(1, 0, NULL)：1=最大计数（二值）、0=初始没货、NULL=默认属性。
     如果先 HAL_ETH_Start_IT 再建信号量，ETH 接收中断可能在信号量创建前到达，
     HAL_ETH_RxCpltCallback 里 osSemaphoreRelease(RxPktSemaphore=NULL) 直接崩 */
  RxPktSemaphore = osSemaphoreNew(1, 0, NULL);
  printf("[ETH] sem=%p\r\n", (void *)RxPktSemaphore);   /* ★断点：信号量建没建成 */
  if (RxPktSemaphore == NULL)
    return ERR_MEM;

  /* 先创建接收任务；它在 MAC 启动前只等待，不访问 DMA 描述符。 */
  RxThread = osThreadNew(ethernetif_thread, netif, &ethernetif_thread_attr);
  if (RxThread == NULL)
  {
    printf("[ETH] rx thread create failed\r\n");
    (void)ethernetif_release_rx();
    return ERR_MEM;
  }

  /* ★★诊断打印（排障）：Start_IT 前读出中断屏蔽状态 + HAL 时基值。
     - PRIMASK=1  → 全局中断被关死（HAL_Delay 等不到 uwTick 涨 → 死循环）
     - BASEPRI≠0  → 优先级 ≤ BASEPRI 的中断被屏蔽
     - uwTick 卡在小值不动 → TIM6 时基中断没在跑
     正常预期：PRIMASK=0、BASEPRI=0、uwTick 每次复位都在增大 */
  printf("[ETH] PRIMASK=%u BASEPRI=%u uwTick=%lu\r\n",
         (unsigned)__get_PRIMASK(), (unsigned)__get_BASEPRI(),
         (unsigned long)HAL_GetTick());

  /* 6. 启动 ETH（★关键动作）
     HAL_ETH_Start_IT 做的事：使能 MAC + DMA，注册接收中断，
     之后 DMA 就开始往 RX_POOL 写数据，每写完一帧调 HAL_ETH_RxCpltCallback */
  if (HAL_ETH_Start_IT(&heth) != HAL_OK)
  {
    printf("[ETH] Start_IT failed: state=%u\r\n", (unsigned)heth.gState);
    (void)ethernetif_release_rx();
    return ERR_IF;
  }
  printf("[ETH] Start_IT done\r\n");                    /* ★断点：ETH DMA 起没起来 */

  /* 7. 初始链接状态上报：上电先把 PHY 链接状态同步给 lwip */
  if (lan8720_link_read())
  {
    printf("[ETH] init link=UP\r\n");                   /* ★断点：上电时链接已建立 */
    netif_set_link_up(netif);   /* 链接建立（网线已插好） */
    netif_set_up(netif);        /* 网口可用（lwip 开始对外服务） */
  }
  else
  {
    printf("[ETH] init link=DOWN\r\n");                 /* ★断点：上电时还没链接 */
    netif_set_link_down(netif);
    netif_set_down(netif);      /* 没插线：先 down，链接线程发现插线后会拉起 */
  }

  printf("[ETH] rx thread created\r\n");                /* ★断点：low_level_init 走完 */
  return ERR_OK;
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
  uint16_t tx_len = (uint16_t)p->tot_len;             /* 实际要发的字节数（可能被补齐放大） */

  /* ★★回收已发完的 Tx 描述符（关键！否则发几帧后就彻底发不出去了）
     HAL 的 Tx 中断处理函数只调 HAL_ETH_TxCpltCallback，**不会**自动回收描述符；
     必须由应用自己调 HAL_ETH_ReleaseTxPacket()，它把 DESC0.OWN=0（DMA 已发完）的包释放掉，
     并回调 HAL_ETH_TxFreeCallback 去 pbuf_free。
     ★为什么放在这里而不是中断回调里：ReleaseTxPacket 会调 pbuf_free（动 lwIP 内存池），
       在中断上下文里做内存池操作不安全；low_level_output 跑在 tcpip 线程里，最稳。
       每次发包前回收一次，DMA 早就把上一帧发完了（微秒级），不会卡住。
     症状对照：不回收 → BuffersInUse 涨到 ETH_TX_DESC_CNT(4) 后，HAL_ETH_Transmit_IT 一直返回
       HAL_ERROR → low_level_output 返回 ERR_IF → 表现为"DHCP 前几帧能发（所以能拿到 IP），
       之后 ARP 应答/ICMP 全部发不出去，PC 永远学不到板子的 MAC"。 */
  HAL_ETH_ReleaseTxPacket(&heth);


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

  /* 短帧补齐说明（评审后删除软件补齐的原因）：
     早期曾在 <60 字节时把帧拷到 4 块轮转静态缓冲里补 0——但轮转下标 tx_pad_idx
     和"哪个 DMA 描述符还在发哪块缓冲"不绑定，存在 DMA 仍读旧缓冲时被 memcpy
     覆盖的隐患。实测已确认 F4 MAC 硬件 CRCPAD 会自动把短帧补到 60 字节并算 FCS，
     软件补齐既多余又危险，已整体移除；pbuf 里多少字节就发多少字节。 */

  /* 填 TxConfig（公共部分同事已配好：校验和/CRC 硬件算，这里只填每帧特有的） */
  TxConfig.Length   = tx_len;          /* 整帧总长（DMA 发这么多字节，补齐后可能 > p->tot_len） */
  TxConfig.TxBuffer = Txbuffer;        /* 分段表 */
  TxConfig.pData    = p;               /* ★把 pbuf 存进 pData，发完回调能取回它来释放 */

#if ETH_DBG_FRAMES
  /* ★★临时排障打印：每次发包打一行，和 [RX] 对照就能看出是不是"收了但不发"。
     ARP 帧额外打印操作码（1=请求 2=应答）和收发双方 IP，用来判断是谁在发、发给谁 */
  {
    static uint32_t tx_dbg_cnt = 0U;
    if ((tx_dbg_cnt < 120U) && (p->len >= 14U))
    {
      const uint8_t *eth = (const uint8_t *)p->payload;
      uint16_t etype = (uint16_t)(((uint16_t)eth[12] << 8) | eth[13]);
      printf("[TX] len=%u type=0x%04X dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)p->tot_len, (unsigned)etype,
             eth[0], eth[1], eth[2], eth[3], eth[4], eth[5],
             eth[6], eth[7], eth[8], eth[9], eth[10], eth[11]);
      if ((etype == 0x0806U) && (p->len >= 42U))
      {
        printf(" arp_op=%u senderIP=%u.%u.%u.%u targetIP=%u.%u.%u.%u"
               " arpSha=%02X:%02X:%02X:%02X:%02X:%02X arpTha=%02X:%02X:%02X:%02X:%02X:%02X",
               (unsigned)(((uint16_t)eth[20] << 8) | eth[21]),
               eth[28], eth[29], eth[30], eth[31],
               eth[38], eth[39], eth[40], eth[41],
               eth[22], eth[23], eth[24], eth[25], eth[26], eth[27],
               eth[32], eth[33], eth[34], eth[35], eth[36], eth[37]);
      }
      printf("\r\n");
      tx_dbg_cnt++;
    }
  }
#endif

  pbuf_ref(p);                         /* ★引用计数 +1：
     调用者（lwip）发完这帧后可能立刻 free 这个 pbuf，
     但 DMA 还在读它的内存！引用 +1 保证 DMA 发完前 pbuf 不会被真正释放 */

  if (HAL_ETH_Transmit_IT(&heth, &TxConfig) != HAL_OK)
  {
#if ETH_DBG_FRAMES
    printf("[TX-FAIL] HAL_ETH_Transmit_IT 返回错误 txlen=%u\r\n", (unsigned)tx_len);
#endif
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
  if (HAL_ETH_ReadData(&heth, (void **)&p) != HAL_OK)   /* 取 pbuf 指针（内部会更新 DMA 描述符） */
  {
    gdbg_rd_fail++;                                    /* ★排障计数：读帧失败次数 */
    return NULL;
  }
  return p;
}

/**
 * @brief  收包线程：等信号量 → 读帧 → 交给 tcpip_thread（netif->input）
 * @note   这就是"带 OS 版"的收包姿势：
 *         裸跑版是主循环里手动轮询；这里线程挂起在信号量上，
 *         接收中断一来 osSemaphoreRelease 唤醒它，不空转不丢包
 *
 * ★★2026-11-10 RBUS 恢复机制：
 *   当 RX_POOL 耗尽导致 DMA RBUS 挂起后，即使池后来有空闲缓冲（pbuf 被 lwip 释放），
 *   DMA 也不会自动恢复——需要手动写 DMARPDR 寄存器"踢"一下 DMA，
 *   让它重新检查描述符可用性并恢复 RX 进程。
 *   本线程每秒超时醒来一次（osSemaphoreAcquire 1000ms），检查 RBUS 标志，
 *   如果检测到则强制调 HAL_ETH_ReadData 触发 ETH_UpdateDescriptor（补缓冲）+ 写 DMARPDR。
 */
static void ethernetif_thread(void *argument)
{
  struct netif *netif = (struct netif *)argument;
  struct pbuf *p = NULL;
#if ETH_DBG_FRAMES
  uint32_t last_irq = 0U, last_err = 0U, last_rdf = 0U;   /* 心跳用：上一次打印时的计数器值 */
#endif

  for (;;)
  {
    /* 挂起等待：没有包时线程睡觉，不占 CPU（★RTOS 的意义所在）
       ★超时给 1 秒而不是永久等待：一是调试时能每秒打一行 [CNT] 心跳，
         判断"没包是没中断还是中断来了但读不出数据"；二是万一中断丢了也能兜底轮询一次。
         正式运行保持 1 秒超时没有副作用（每次醒来读一遍没包就立刻睡回去） */
    osSemaphoreAcquire(RxPktSemaphore, 1000U);
    if (heth.gState != HAL_ETH_STATE_STARTED)
      continue;

    /* ★评审要求：收发包用同一个任务上下文串行化 TX 提交/回收。
       本线程每 ~1s 醒一次（无论有没有收到帧），顺手回收已发完的 Tx 描述符和 pbuf——
       否则"最后一批已发帧"会一直占着描述符/pbuf 直到下一次发包。
       和 low_level_output 开头的回收形成双保险：发包路径即时回收，空闲时本线程兜底回收。 */
    HAL_ETH_ReleaseTxPacket(&heth);

#if ETH_DBG_FRAMES
    /* 心跳：每秒打印一次中断/错误/读失败计数 + pool 使用情况（只在有变化时打，避免刷屏） */
    if ((gdbg_rx_irq != last_irq) || (gdbg_err_irq != last_err) || (gdbg_rd_fail != last_rdf))
    {
      printf("[CNT] rxirq=%lu errirq=%lu rdfail=%lu errcode=0x%08lX state=%u"
             " pool: alloc=%lu null=%lu free=%lu\r\n",
             (unsigned long)gdbg_rx_irq, (unsigned long)gdbg_err_irq,
             (unsigned long)gdbg_rd_fail, (unsigned long)gdbg_err_code,
             (unsigned)heth.gState,
             (unsigned long)gdbg_pool_alloc, (unsigned long)gdbg_pool_null,
             (unsigned long)gdbg_pool_free);
      last_irq = gdbg_rx_irq; last_err = gdbg_err_irq; last_rdf = gdbg_rd_fail;
    }
#endif

    /* 醒来后把积压帧全部读完（中断可能一次性来了多帧，信号量只是"有货"通知）
       每帧都通过 netif->input(=tcpip_input) 扔给内核线程处理 */
    while ((p = low_level_input(netif)) != NULL)
    {
      /* ★★临时排障打印（ETH_DBG_FRAMES=1 时启用）：抓 ping 期间的帧流向，
         确认 ① 板子到底有没有收到帧 ② 收到的是不是 ARP/ICMP
         调试完把文件顶部的 ETH_DBG_FRAMES 改回 0 即可关闭，不留痕迹。 */
#if ETH_DBG_FRAMES
      {
        static uint32_t rx_dbg_cnt = 0U;
        if ((rx_dbg_cnt < 120U) && (p->len >= 14U))
        {
          const uint8_t *eth = (const uint8_t *)p->payload;
          uint16_t etype = (uint16_t)(((uint16_t)eth[12] << 8) | eth[13]);
          printf("[RX] type=0x%04X len=%u dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                 (unsigned)etype, (unsigned)p->len,
                 eth[0], eth[1], eth[2], eth[3], eth[4], eth[5],
                 eth[6], eth[7], eth[8], eth[9], eth[10], eth[11]);
          rx_dbg_cnt++;
        }
      }
#endif
      if (netif->input(p, netif) != ERR_OK)   /* 内核处理失败（内存不够等） */
        pbuf_free(p);                          /* 自己释放，不能丢 */
    }

    /* ★★RBUS 恢复：检查是否发生过 RBUS（DMA RX 挂起）
       即使池扩大后 RBUS 理论上不会再发生，这里作为安全网：
       如果 lwip 因某种原因暂持大量 pbuf 导致池再次耗尽，RBUS 恢复能自动救回，
       不至于像之前一样永久死锁（CONNACK 永远收不到）。 */
    if (gdbg_err_code & 0x0080U)  /* ETH_DMASR_RBUS bit */
    {
      gdbg_err_code = 0U;  /* 清除标志，避免重复恢复 */

      /* 步骤1：强制调 HAL_ETH_ReadData 触发 ETH_UpdateDescriptor。
         如果 RxBuildDescCnt > 0（之前有描述符被消费但未补缓冲），
         ETH_UpdateDescriptor 会尝试从池里取新缓冲补上，并写 DMARPDR 恢复 DMA。
         此时如果池有空闲（pbuf 已被 lwip 释放），恢复成功。 */
      void *dummy_p = NULL;
      HAL_ETH_ReadData(&heth, &dummy_p);

      /* 步骤2：无论步骤1是否成功，手动写 DMARPDR "踢"DMA。
         DMA 收到此写入后会重新检查描述符环——如果有 OWN=1 的描述符可用，
         RX 进程立即从 Suspended 恢复到 Running。
         写入值不影响行为（RM0090: "any value"），用 0 即可。 */
      __DMB();  /* 确保前面的描述符 OWN 位写入对 DMA 可见 */
      WRITE_REG(heth.Instance->DMARPDR, 0U);
      gdbg_rbus_recover++;
      printf("[ETH] RBUS recovery #%lu (DMARPDR kicked, pool: alloc=%lu null=%lu free=%lu)\r\n",
             (unsigned long)gdbg_rbus_recover,
             (unsigned long)gdbg_pool_alloc, (unsigned long)gdbg_pool_null,
             (unsigned long)gdbg_pool_free);
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

  return low_level_init(netif);
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
  gdbg_pool_free++;                          /* ★诊断：跟踪池归还 */
}

/** Rx 缓冲分配回调：从 RX_POOL 取一块给 DMA 描述符
    ★零拷贝第一环：DMA 启动前/运行中缺缓冲时，HAL 调这个函数拿一块内存，
    把地址给 DMA 描述符（*buff），DMA 直接往这块内存写网线来的数据 */
void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
  struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
  gdbg_pool_alloc++;                                /* ★诊断：跟踪池分配 */
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
    *buff = NULL;   /* ★池空了！这是 RBUS 死锁的根因——DMA 描述符拿不到新缓冲 */
    gdbg_pool_null++;                                /* ★诊断：跟踪池耗尽次数 */
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
  gdbg_rx_irq++;                        /* ★排障计数：DMA 收到一帧 */
  osSemaphoreRelease(RxPktSemaphore);   /* 给信号量（从中断调，CMSIS-RTOS 支持） */
}

/** 发送完成回调（描述符回收由 low_level_output 里的 HAL_ETH_ReleaseTxPacket 负责，这里留空） */
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth)
{
  (void)heth;
}

/* ETH中断只记录错误，DMA恢复与日志由收包线程处理，避免阻塞SD轮询。 */
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth)
{
  gdbg_err_irq++;
  gdbg_err_code = heth->DMAErrorCode;
}
