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

// 一、头文件
#include <stdio.h>          // printf打印，重定向在main.c
#include "stm32f4xx_hal.h"     // HAL库，所有ETH操作API
#include "lwip/opt.h"          // LwIP配置，所有LwIP文件第一个必须包含
#include "lwip/def.h"          // 字节序工具宏
#include "lwip/mem.h"          // LwIP堆内存
#include "lwip/memp.h"         // 内存池API，零拷贝池的基础
#include "lwip/pbuf.h"         // pbuf包结构，网络数据包的载体
#include "lwip/timeouts.h"     // LwIP超时管理
#include "lwip/stats.h"        // 统计信息，关了就是空实现
#include "netif/ethernet.h"    // 以太网层，MAC地址长度、最大帧长
#include "netif/etharp.h"      // ARP地址解析
#include "cmsis_os.h"          // FreeRTOS封装，信号量、线程
#include <string.h>            // memcpy内存拷贝
#include <stddef.h>            // offsetof算结构体成员偏移

#include "ethernetif.h"        // 本模块头文件

// 二、外部对象，同事的eth.c里定义好了，我们直接借用，不能重复定义
extern ETH_HandleTypeDef heth;                       // ETH句柄，所有硬件操作都靠它
extern ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT];  // 接收DMA描述符，DMA知道往哪写
extern ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT];  // 发送DMA描述符，DMA知道读哪发
extern ETH_TxPacketConfig TxConfig;                  // 发送公共配置，校验和、自动补CRC

// 三、板级宏定义，LAN8720A PHY
#define LAN8720_PHY_ADDRESS         ((uint8_t)0x00) // PHY地址，硬件引脚决定，默认0

// LAN8720寄存器，标准802.3通用，ifndef防止和HAL头里的重复定义
#ifndef PHY_BSR                               // 没定义过BSR才定义，防重复
#define PHY_BSR                     ((uint16_t)0x01)   // 基本状态寄存器，看链路状态
#endif                                          // 条件编译结束
#define PHY_IDR1                    ((uint16_t)0x02)   // 厂商ID高16位
#define PHY_IDR2                    ((uint16_t)0x03)   // 厂商ID低16位
#define PHY_BMCR                    ((uint16_t)0x00)   // 基本控制寄存器，软复位、自动协商

// LAN8720的固定ID，读出来对得上才说明对面真的是这个芯片
#define LAN8720_IDR1_VAL            ((uint16_t)0x0007)   // LAN8720厂商ID高16位期望值
#define LAN8720_IDR2_VAL            ((uint16_t)0xC0F1)   // LAN8720厂商ID低16位期望值

#ifndef PHY_LINKED_STATUS                    // 没定义过才定义，防重复
#define PHY_LINKED_STATUS           ((uint16_t)0x0004) // 状态寄存器bit2，1就是链路通
#endif                                         // 条件编译结束
#define BMCR_SOFT_RESET             ((uint16_t)0x8000) // 控制寄存器bit15，写1软复位
#define BMCR_AUTONEGO_EN            ((uint16_t)0x1000) // bit12，1就是开自动协商

// LwIP网卡名字，调试显示用
#define IFNAME0 's'                            // 网卡名第一个字符
#define IFNAME1 't'                            // 网卡名第二个字符，合起来"st"

// 零拷贝接收缓冲数量，必须大于等于DMA描述符数，原来6个会饿死DMA，改成16个留余量
#define ETH_RX_BUFFER_CNT           16         // 零拷贝接收池16个缓冲

// 以太网最小帧长，不足的补0，MAC硬件本身也会自动补，这里是双保险
#define ETH_MIN_FRAME_LEN           60U        // 以太网最小帧长60字节

// 四、零拷贝接收池，核心数据结构，DMA直接往LwIP的pbuf里写，不用CPU搬数据
// 每个接收缓冲的结构，上面是自定义pbuf头，下面是数据区，32字节对齐满足DMA要求
typedef struct                                // 接收缓冲结构体定义开始
{
  struct pbuf_custom pbuf_custom;                            // LwIP自定义pbuf头，存释放函数
  uint8_t buff[(ETH_RX_BUF_SIZE + 31) & ~31] __ALIGNED(32);  // 32字节对齐数据区，DMA直接写
} RxBuff_t;                                   // 别名RxBuff_t定义结束

// 声明LwIP内存池，每个元素是RxBuff_t，这块内存DMA和LwIP共用，零拷贝的关键
LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");   // 声明接收内存池，16个RxBuff_t

// 收包信号量，中断里释放，收包线程获取，只要通知有货不用传数据，二值最轻量
static osSemaphoreId_t RxPktSemaphore;        // 收包信号量，中断放线程取
static osThreadId_t RxThread;                 // 收包线程句柄
static uint8_t RxPoolInitialized;             // 接收池初始化过没有的标志

// 帧打印调试开关，1打开打印收发帧，0关闭，查链路问题时改1
#define ETH_DBG_FRAMES   0                     // 调试打印开关，0关1开

// 排障计数器，诊断DMA和内存池问题用，volatile防优化，量很小
volatile uint32_t gdbg_rx_irq   = 0U;   // 收包中断次数
volatile uint32_t gdbg_err_irq  = 0U;   // 错误中断次数
volatile uint32_t gdbg_rd_fail  = 0U;   // 读帧失败次数
volatile uint32_t gdbg_err_code = 0U;   // 最近DMA错误码
volatile uint32_t gdbg_pool_alloc = 0U; // 接收池分配次数
volatile uint32_t gdbg_pool_null  = 0U; // 分配失败次数，池空
volatile uint32_t gdbg_pool_free  = 0U; // 接收池释放次数
volatile uint32_t gdbg_rbus_recover = 0U; // RBUS错误恢复尝试次数

// 收包线程属性，名字ethif，栈1024，高优先级，栈要够放printf和tcpip_input
static const osThreadAttr_t ethernetif_thread_attr = {   // 收包线程属性初始化
  .name       = "ethif",                    // 线程名
  .stack_size = 1024,                       // 栈1024字节
  .priority   = osPriorityHigh,             // 高优先级
};                                          // 属性结构结束

// 五、内部函数前向声明，先告诉编译器有这些函数
static err_t low_level_init(struct netif *netif);           // 网卡底层初始化
static err_t  low_level_output(struct netif *netif, struct pbuf *p);  // 发一帧
static struct pbuf *low_level_input(struct netif *netif);   // 从DMA取一帧
static void ethernetif_thread(void *argument);              // 收包线程，等信号量、读帧、喂内核
void pbuf_free_custom(struct pbuf *p);                      // 自定义pbuf释放，还回内存池

// PHY读写函数，封装HAL的MDIO操作
static uint8_t lan8720_read_reg(uint16_t reg, uint32_t *val);   // 读PHY寄存器
static uint8_t lan8720_write_reg(uint16_t reg, uint32_t val);   // 写PHY寄存器

// 函数：lan8720_read_reg —— 读PHY寄存器
// 功能：通过STM32 HAL的MDIO接口读LAN8720A PHY的指定寄存器：拿ETH句柄、发读命令、等待完成；注意MDIO由HAL管理，必须在ETH外设初始化之后才能调，否则读不到
// 参数：reg —— PHY寄存器地址（如BSR是1、BCR是0）；val —— 出参，读到的32位值存这里
// 返回：uint8_t，0代表读成功；1代表读失败（HAL超时或参数错）
static uint8_t lan8720_read_reg(uint16_t reg, uint32_t *val)   // 读PHY寄存器，0成功1失败
{
  if (HAL_ETH_ReadPHYRegister(&heth, LAN8720_PHY_ADDRESS, reg, val) != HAL_OK)   // HAL读寄存器失败判断
    return 1;      // 读失败返回1
  return 0;        // 成功返回0
}

// 函数：lan8720_write_reg —— 写PHY寄存器
// 功能：通过HAL的MDIO接口往LAN8720A PHY指定寄存器写32位值：比如软复位写BCR的bit15、开自动协商写BCR相关位；同样必须在ETH初始化之后才能调
// 参数：reg —— 目标PHY寄存器地址；val —— 要写入的32位值
// 返回：uint8_t，0代表写成功；1代表写失败（HAL超时或参数错）
static uint8_t lan8720_write_reg(uint16_t reg, uint32_t val)   // 写PHY寄存器，0成功1失败
{
  if (HAL_ETH_WritePHYRegister(&heth, LAN8720_PHY_ADDRESS, reg, val) != HAL_OK)   // HAL写寄存器失败判断
    return 1;      // 失败返回1
  return 0;        // 成功返回0
}

// 函数：lan8720_link_read —— 读PHY链路状态（对外接口）
// 功能：读PHY的BSR（基本状态寄存器，地址1）的bit2链路状态位，判断网线插没插、物理链路通不通；ethernet.c的链接线程每秒轮询一次这个函数感知插拔
// 参数：无
// 返回：uint8_t，1代表链路通（网线插好协商完成）；0代表链路断（没插线或协商中）
uint8_t lan8720_link_read(void)                 // 对外接口：读链路状态
{
  uint32_t val = 0;                            // 寄存器值临时变量
  static uint8_t last_fail = 0;   // 只在成功失败转变时打印，避免刷屏
  if (lan8720_read_reg(PHY_BSR, &val) != 0)    // 读状态寄存器失败
  {
    if (!last_fail) { printf("[PHY] link read FAIL (MDIO no response)\r\n"); last_fail = 1; }   // 首次失败打印并标记
    return 0;                     // 读失败按未链接处理
  }
  if (last_fail) { printf("[PHY] link read OK again (BSR=0x%04X)\r\n", (uint16_t)val); last_fail = 0; }   // 恢复了打印一下
  uint8_t cur = (val & PHY_LINKED_STATUS) ? 1 : 0;   // BSR bit2为1就是链接建立
  static uint8_t last_link = 0xFF;             // 上次链路状态，0xFF保证首查必打印
  if (cur != last_link)                        // 链路状态变了
  {
    printf("[PHY] link %s (BSR=0x%04X bit2=%d)\r\n", cur ? "UP" : "DOWN", (uint16_t)val, (val & 0x0004) ? 1 : 0);   // 打印变化
    last_link = cur;                            // 更新记录
  }
  return cur;                                   // 返回当前链路状态
}

// 函数：lan8720_init —— LAN8720A探测初始化
// 功能：PHY上电初始化全套：先读两个ID寄存器验证芯片活着（读到0xFFFF说明MDIO没通，读出固定值0x0007/0xC0F1才是真LAN8720），然后写BCR软复位芯片，再开自动协商让它和对端自动谈速率双工
// 参数：无
// 返回：uint8_t，0代表初始化成功；1代表失败（ID读不到或复位超时），调用方可重试
static uint8_t lan8720_init(void)              // LAN8720探测初始化，0成功1失败
{
  uint32_t idr1 = 0, idr2 = 0, val = 0;        // ID和寄存器值临时变量
  uint16_t timeout = 0;                        // 超时计数器

  // 第一步读PHY ID，同时验证MDIO通路，读不到固定值就是硬件没接好
  if (lan8720_read_reg(PHY_IDR1, &idr1) != 0) { printf("[PHY] read IDR1 FAIL (MDIO no response?)\r\n"); return 1; }   // 读IDR1失败
  if (lan8720_read_reg(PHY_IDR2, &idr2) != 0) { printf("[PHY] read IDR2 FAIL (MDIO no response?)\r\n"); return 1; }   // 读IDR2失败
  // 打印实际ID，0xFFFF就是MDIO没通，固定值才是LAN8720活着
  printf("[PHY] IDR1=0x%04X IDR2=0x%04X (expect 0x0007/0xC0F1)\r\n",   // 打印读到的ID
         (uint16_t)idr1, (uint16_t)idr2);      // 两个ID值      // 打印读到的ID
  if ((idr1 != LAN8720_IDR1_VAL) || (idr2 != LAN8720_IDR2_VAL))   // ID对不上
  {
    printf("[PHY] ID MISMATCH -> PHY not responding (check power/clock/reset)\r\n");   // 提示硬件问题
    return 1;                                 // 返回失败
  }
  printf("[PHY] LAN8720A detected, MDIO OK\r\n");   // 确认识别到芯片

  // 第二步软复位，走MDIO不依赖硬件复位引脚，任何时候都能做
  if (lan8720_write_reg(PHY_BMCR, BMCR_SOFT_RESET) != 0) { printf("[PHY] soft-reset write FAIL\r\n"); return 1; }   // 写复位位失败
  // 等硬件自动清复位位，加超时防止死等
  do {                                        // 轮询循环开始
    if (lan8720_read_reg(PHY_BMCR, &val) != 0) { printf("[PHY] soft-reset poll read FAIL\r\n"); return 1; }   // 读复位位失败
    timeout++;                                 // 计数加1
  } while ((val & BMCR_SOFT_RESET) && (timeout < 1000));   // 复位位还在且没超时就继续等
  if (val & BMCR_SOFT_RESET) { printf("[PHY] soft-reset TIMEOUT\r\n"); return 1; }   // 等了1000次还没清
  printf("[PHY] soft-reset done\r\n");        // 复位完成日志

  // 第三步开自动协商，让PHY和对端自动定速率和双工，取共同最高能力
  if (lan8720_write_reg(PHY_BMCR, BMCR_AUTONEGO_EN | 0x0100) != 0) { printf("[PHY] autonego write FAIL\r\n"); return 1; }   // 开协商失败
  printf("[PHY] autonego started\r\n");       // 协商已发起日志

  // 等协商完成，状态寄存器bit5，最多等3秒，有些设备不响应就超时强制100M全双工
  timeout = 0;                                // 超时计数清零
  do {                                        // 轮询循环开始
    osDelay(10);                              // 睡10毫秒再查
    if (lan8720_read_reg(PHY_BSR, &val) != 0) { printf("[PHY] autonego poll FAIL\r\n"); break; }   // 读失败跳出
    timeout++;                                // 计数加1
  } while (((val & 0x0020) == 0) && (timeout < 300));   // bit5是协商完成标志
  if (val & 0x0020)                           // 协商完成
  {
    printf("[PHY] autonego done (BSR=0x%04X)\r\n", (uint16_t)val);   // 打印完成
  }
  else                                        // 协商超时
  {
    printf("[PHY] autonego TIMEOUT (BSR=0x%04X), forcing 100M FD\r\n", (uint16_t)val);   // 打印超时
    // 强制100M全双工，bit13=100M，bit8=全双工，bit12=0关协商，值0x2100
    lan8720_write_reg(PHY_BMCR, 0x2100);      // 写强制模式
    osDelay(100);                             // 等100毫秒生效
    lan8720_read_reg(PHY_BSR, &val);          // 回读状态确认
    printf("[PHY] after force: BSR=0x%04X link=%d\r\n", (uint16_t)val, (val & 0x0004) ? 1 : 0);   // 打印强制后状态
  }

  return 0;   // 全部成功
}

// 函数：get_stm32_mac —— 用芯片UID生成MAC地址
// 功能：读STM32F407芯片出厂唯一ID（96位UID寄存器），截取其中48位拼成本网卡的MAC地址；每颗芯片UID都不同，多块板子MAC天然不冲突，省去手动烧MAC的麻烦
// 参数：mac —— 输出数组指针，至少6字节，生成的MAC地址写在这里
// 返回：void，无返回值
static void get_stm32_mac(uint8_t *mac)         // 用UID生成MAC地址
{
  uint32_t uid[3];    // 96位UID，3个32位寄存器，出厂固化
  uint32_t sum = 0, xor = 0;   // 累加和、异或和
  uint8_t  buf[12];           // UID展开成12字节
  uint8_t  i;                // 循环变量

  // STM32F4 UID固定基地址0x1FFF7A10，直接读三个寄存器
  uid[0] = *(uint32_t *)(0x1FFF7A10);          // 读UID第1个字
  uid[1] = *(uint32_t *)(0x1FFF7A14);          // 读UID第2个字
  uid[2] = *(uint32_t *)(0x1FFF7A18);          // 读UID第3个字

  // 把12个字节拆出来，用来算校验和异或
  for (i = 0; i < 4; i++) { buf[i]   = (uint8_t)(uid[0] >> (8 * i)); }   // uid[0]拆4字节
  for (i = 0; i < 4; i++) { buf[4+i] = (uint8_t)(uid[1] >> (8 * i)); }   // uid[1]拆4字节
  for (i = 0; i < 4; i++) { buf[8+i] = (uint8_t)(uid[2] >> (8 * i)); }   // uid[2]拆4字节

  // 后两个字节用累加和、异或和，不同芯片大概率不一样
  for (i = 0; i < 12; i++) { sum += buf[i]; }   // 所有字节累加
  for (i = 0; i < 12; i++) { xor ^= buf[i]; }   // 所有字节异或

  // 前4字节直接用UID第一个字的4个字节
  mac[0] = (uint8_t)((uid[0] & 0xFF000000) >> 24);   // MAC第0字节：uid0最高字节
  mac[1] = (uint8_t)((uid[0] & 0x00FF0000) >> 16);   // MAC第1字节：uid0次高字节
  mac[2] = (uint8_t)((uid[0] & 0x0000FF00) >> 8);    // MAC第2字节：uid0次低字节
  mac[3] = (uint8_t)(uid[0] & 0x000000FF);           // MAC第3字节：uid0最低字节
  mac[4] = (uint8_t)(sum & 0xFF);   // 累加和
  mac[5] = (uint8_t)(xor & 0xFF);   // 异或和

  // MAC首字节规则，bit1置1标记本地管理地址（不是厂商注册OUI），bit0清0保证单播
  mac[0] = (uint8_t)((mac[0] & 0xFCU) | 0x02U);     // bit1置1本地管理，bit0清0单播
}                                                   // 生成MAC函数结束

// 函数：ethernetif_release_rx —— 回收接收侧资源
// 功能：删除接收线程和接收信号量、清空DMA描述符队列，释放网卡收包占用的资源；某项删除失败会保留句柄留给下次重试，防止反复建删把系统搞挂；只在DMA还没启动时用，跑起来就不能删
// 参数：无（操作全局接收线程句柄和信号量）
// 返回：err_t，ERR_OK代表全部回收干净；失败错误码代表有残留，下次再试
static err_t ethernetif_release_rx(void)       // 回收接收资源，删线程信号量
{
  if (RxThread != NULL)                        // 收包线程还活着
  {
    if (osThreadTerminate(RxThread) != osOK)   // 删线程失败
      return ERR_IF;                            // 返回接口错误
    RxThread = NULL;                            // 句柄清空
  }
  if (RxPktSemaphore != NULL)                   // 信号量还活着
  {
    if (osSemaphoreDelete(RxPktSemaphore) != osOK)   // 删信号量失败
      return ERR_IF;                            // 返回接口错误
    RxPktSemaphore = NULL;                      // 句柄清空
  }
  return ERR_OK;                                // 回收成功
}

// 函数：low_level_init —— 网卡底层硬件初始化
// 功能：LwIP加网卡前的底层初始化：设MTU1500、把UID生成的MAC写进netif、初始化接收池信号量、调lan8720_init探PHY、初始化HAL ETH外设、填DMA描述符开收发、建接收线程；返回ERR_OK网卡才能加入协议栈
// 参数：netif —— LwIP网卡结构指针，MTU、MAC地址、链路状态都会写进它
// 返回：err_t，ERR_OK代表硬件全部就绪；ERR_IF等错误码代表PHY失败或ETH初始化失败，netif_add会中止
static err_t low_level_init(struct netif *netif)   // 网卡底层初始化
{
  uint8_t macaddress[6] = {0};                 // MAC地址临时数组清零

  if (heth.gState != HAL_ETH_STATE_READY || ethernetif_release_rx() != ERR_OK)   // HAL没就绪或回收失败
    return ERR_IF;                              // 返回接口错误

  // 第一步配netif基本参数，MTU1500，开广播和ARP，不标链路UP，链路由链接线程管
  netif->mtu = ETH_MAX_PAYLOAD;                 // MTU1500字节
  netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;   // 开广播和ARP标志

  // 第二步生成MAC，同时写LwIP和HAL寄存器，两边必须一致不然断网
  get_stm32_mac(macaddress);                    // 用UID生成MAC
  netif->hwaddr_len = ETH_HWADDR_LEN;            // MAC长度6字节
  memcpy(netif->hwaddr, macaddress, 6);          // LwIP侧MAC，ARP报文用

  // F4 HAL没有SetMACAddress接口，直接写MAC地址寄存低32位（小端拼接）
  ETH->MACA0LR = (uint32_t)((uint32_t)macaddress[0] |   // MAC字节0放最低位
                ((uint32_t)macaddress[1] << 8) |   // MAC字节1左移8位
                ((uint32_t)macaddress[2] << 16) |  // MAC字节2左移16位
                ((uint32_t)macaddress[3] << 24));  // MAC字节3左移24位
  ETH->MACA0HR = (uint32_t)((uint32_t)macaddress[4] |    // 高32位放MAC字节4
                ((uint32_t)macaddress[5] << 8) | 0x80000000U);   // bit31=AE地址有效，必须置1

  // 第三步初始化零拷贝接收池，必须在启动DMA前，DMA一启动就要从池里取缓冲
  if (!RxPoolInitialized)                       // 池还没初始化过
  {
    LWIP_MEMPOOL_INIT(RX_POOL);                // 初始化接收内存池
    RxPoolInitialized = 1;                      // 标记已初始化
  }

  // 第四步初始化PHY，探测、软复位、协商，失败就标记网卡down不启动ETH
  if (lan8720_init() != 0)                      // PHY初始化失败
  {
    netif_set_link_down(netif);                 // 链路标记断
    netif_set_down(netif);                      // 网口标记down
    return ERR_IF;                              // 返回接口错误
  }

  // 第五步建收包信号量，必须先建信号量再启动ETH，不然中断来了句柄是空的直接崩
  RxPktSemaphore = osSemaphoreNew(1, 0, NULL);  // 建二值信号量，初值0
  printf("[ETH] sem=%p\r\n", (void *)RxPktSemaphore);   // 打印信号量句柄
  if (RxPktSemaphore == NULL)                   // 建失败
    return ERR_MEM;                             // 返回内存错误

  // 先建收包线程，MAC启动前它只等信号量，不碰DMA描述符
  RxThread = osThreadNew(ethernetif_thread, netif, &ethernetif_thread_attr);   // 建收包线程
  if (RxThread == NULL)                         // 建失败
  {
    printf("[ETH] rx thread create failed\r\n");   // 打印失败
    (void)ethernetif_release_rx();              // 回收已建的资源
    return ERR_MEM;                             // 返回内存错误
  }

  // 诊断打印，检查全局中断有没有被关、HAL时基有没有在跑
  printf("[ETH] PRIMASK=%u BASEPRI=%u uwTick=%lu\r\n",   // 打印中断和时基诊断
         (unsigned)__get_PRIMASK(), (unsigned)__get_BASEPRI(),   // 打印中断屏蔽状态
         (unsigned long)HAL_GetTick());        // 打印时基tick

  // 第六步启动ETH，使能MAC和DMA，或者说DMA描述符
  if (HAL_ETH_Start_IT(&heth) != HAL_OK)       // 启动失败判断
  {
    printf("[ETH] Start_IT failed: state=%u\r\n", (unsigned)heth.gState);   // 打印失败状态
    (void)ethernetif_release_rx();              // 回收资源
    return ERR_IF;                              // 返回接口错误
  }
  printf("[ETH] Start_IT done\r\n");            // 启动成功日志

  // 第七步同步初始链路状态，上电就把PHY的链路状态告诉LwIP
  if (lan8720_link_read())                      // 上电时链路就是通的
  {
    printf("[ETH] init link=UP\r\n");           // 打印链路UP
    netif_set_link_up(netif);   // 链路UP，网线插好
    netif_set_up(netif);        // 网口UP，LwIP开始服务
  }
  else                                          // 上电没插线
  {
    printf("[ETH] init link=DOWN\r\n");         // 打印链路DOWN
    netif_set_link_down(netif);                 // 链路标记断
    netif_set_down(netif);      /* 没插线：先 down，链接线程发现插线后会拉起 */   // 网口down等链接线程拉起
  }

  printf("[ETH] rx thread created\r\n");        // 收包线程就绪日志
  return ERR_OK;                                // 初始化成功
}

// 函数：low_level_output —— 发送一帧（LwIP调它）
// 功能：LwIP要发IP包时通过netif的linkoutput指针调本函数：不拷贝数据，直接把pbuf指针挂给DMA发送描述符让DMA搬内存，发完后HAL的TxFree回调再真正释放pbuf——零拷贝省CPU省内存
// 参数：netif —— 发包用的网卡结构指针；p —— LwIP拼好的待发数据包pbuf链头指针
// 返回：err_t，ERR_OK代表帧已交给DMA发送；ERR_TIMEOUT等错误码代表描述符不够或发送失败
static err_t low_level_output(struct netif *netif, struct pbuf *p)   // 发一帧，LwIP调它
{
  uint32_t i = 0U;                     // 当前pbuf链第几段
  struct pbuf *q = NULL;               // 遍历pbuf链的游标
  ETH_BufferTypeDef Txbuffer[ETH_TX_DESC_CNT] = {0};  // DMA分段表，最多4段
  uint16_t tx_len = (uint16_t)p->tot_len;             // 整帧总长度

  // 关键：发包前回收已发完的发送描述符，不然发4个就满了彻底发不出去
  // HAL中断不会自动回收，必须自己调，放这里因为在tcpip线程，动内存池安全
  HAL_ETH_ReleaseTxPacket(&heth);              // 回收发完的TX描述符

  // 遍历pbuf链，把每段指针和长度填进DMA分段表，DMA自动串成一帧
  for (q = p; q != NULL; q = q->next)          // 遍历pbuf链每一段
  {
    if (i >= ETH_TX_DESC_CNT)          // 段数超过4个发不了
      return ERR_IF;                   // 返回接口错误

    Txbuffer[i].buffer = q->payload;   // 只给指针不拷数据，零拷贝
    Txbuffer[i].len    = q->len;        // 这一段的长度

    // 把分段表串成链表，最后一段next为NULL
    if (i > 0)                         // 不是第一段
      Txbuffer[i-1].next = &Txbuffer[i];   // 前一段指向这一段
    if (q->next == NULL)               // 这是最后一段
      Txbuffer[i].next = NULL;          // 链表尾巴置空

    i++;                                        // 段序号加1，准备填下一段
  }                                             // pbuf遍历结束

  // 短帧不用软件补，MAC硬件CRCPAD自动补，软件补有缓冲覆盖隐患，已移除
  // 填每帧的发送配置，公共的CRC、校验和硬件算，这里只填长度和缓冲
  TxConfig.Length   = tx_len;          // 整帧总长
  TxConfig.TxBuffer = Txbuffer;        // 分段表
  TxConfig.pData    = p;               // 把pbuf存这里，发完回调能取回释放

#if ETH_DBG_FRAMES                          // 调试开关开了才编译打印块
  // 调试打印，发包打一行，和收包对照，ARP额外打操作码和IP
  {                                         // 打印块开始
    static uint32_t tx_dbg_cnt = 0U;        // 已打印帧数计数
    if ((tx_dbg_cnt < 120U) && (p->len >= 14U))   // 只打前120个完整帧
    {                                       // 打印条件体开始
      const uint8_t *eth = (const uint8_t *)p->payload;   // 帧数据指针
      uint16_t etype = (uint16_t)(((uint16_t)eth[12] << 8) | eth[13]);   // 取以太网类型字段
      printf("[TX] len=%u type=0x%04X dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X",   // 打帧头
             (unsigned)p->tot_len, (unsigned)etype,    // 总长和类型
             eth[0], eth[1], eth[2], eth[3], eth[4], eth[5],   // 目的MAC6字节
             eth[6], eth[7], eth[8], eth[9], eth[10], eth[11]);   // 源MAC6字节
      if ((etype == 0x0806U) && (p->len >= 42U))   // 是ARP帧且够长
      {                                     // ARP打印体开始
        printf(" arp_op=%u senderIP=%u.%u.%u.%u targetIP=%u.%u.%u.%u"   // 打ARP操作码和IP
               " arpSha=%02X:%02X:%02X:%02X:%02X:%02X arpTha=%02X:%02X:%02X:%02X:%02X:%02X",   // 打ARP两端MAC
               (unsigned)(((uint16_t)eth[20] << 8) | eth[21]),   // ARP操作码
               eth[28], eth[29], eth[30], eth[31],   // 发送方IP
               eth[38], eth[39], eth[40], eth[41],   // 目标方IP
               eth[22], eth[23], eth[24], eth[25], eth[26], eth[27],   // 发送方MAC
               eth[32], eth[33], eth[34], eth[35], eth[36], eth[37]);  // 目标方MAC
      }                                     // ARP打印体结束
      printf("\r\n");                       // 打印换行
      tx_dbg_cnt++;                          // 计数加1
    }                                       // 打印条件体结束
  }                                         // 打印块结束
#endif                                      // 调试条件编译结束

  pbuf_ref(p);                         // 引用计数加1，DMA发完前LwIP不能释放这个pbuf

  if (HAL_ETH_Transmit_IT(&heth, &TxConfig) != HAL_OK)   // 提交DMA发送失败
  {
#if ETH_DBG_FRAMES                    // 调试开关开了才打印
    printf("[TX-FAIL] HAL_ETH_Transmit_IT 返回错误 txlen=%u\r\n", (unsigned)tx_len);   // 打印发送失败
#endif                                // 条件编译结束
    pbuf_free(p);                      // 发送失败，把加的引用还回去
    return ERR_IF;                     // 返回接口错误
  }

  return ERR_OK;   // 已提交DMA，发完自动回调释放pbuf
}                                     // 发送函数结束

// 函数：low_level_input —— 从DMA收队列取一帧
// 功能：收包线程调它：检查DMA接收描述符有没有新帧，有就把DMA直接写好的那个pbuf取出来还给协议栈——整个接收路径DMA直接写进pbuf，这里不搬任何数据，零拷贝收包的最后一环
// 参数：netif —— 收包用的网卡结构指针（当前未直接使用）
// 返回：struct pbuf*，收到的帧pbuf指针，没收到返回NULL
static struct pbuf *low_level_input(struct netif *netif)   // 从DMA收队列取一帧
{
  struct pbuf *p = NULL;                       // 结果pbuf指针
  if (HAL_ETH_ReadData(&heth, (void **)&p) != HAL_OK)   // 取pbuf，内部更新DMA描述符
  {
    gdbg_rd_fail++;                            // 读帧失败计数
    return NULL;                               // 失败返回空
  }
  return p;                                    // 成功返回DMA直接写的pbuf
}                                              // 取帧函数结束

// 函数：ethernetif_thread —— 收包线程
// 功能：FreeRTOS线程入口，收包主循环：无限等接收信号量（中断来了就醒），把收到的每帧通过netif的input指针送进LwIP协议栈；每次1秒超时醒来做RBUS兜底检查——DMA挂起就手动踢寄存器恢复，防止网卡假死
// 参数：argument —— FreeRTOS线程参数，没用到
// 返回：void，无返回值，死循环不退出
static void ethernetif_thread(void *argument)  // 收包线程入口
{
  struct netif *netif = (struct netif *)argument;   // 线程参数就是网卡对象
  struct pbuf *p = NULL;                       // 当前帧pbuf
#if ETH_DBG_FRAMES                            // 调试开关开了才编译变量
  uint32_t last_irq = 0U, last_err = 0U, last_rdf = 0U;   // 心跳打印，记录上次值
#endif                                         // 条件编译结束

  for (;;)                                     // 线程死循环
  {                                            // 循环体开始
    // 等信号量，没包就睡觉不占CPU，超时1秒，调试打心跳、中断丢了也能兜底
    osSemaphoreAcquire(RxPktSemaphore, 1000U);  // 等信号量最多1秒
    if (heth.gState != HAL_ETH_STATE_STARTED)   // ETH没在跑
      continue;                                 // 跳过本轮继续等

    // 空闲时兜底回收发送描述符，和发包前的回收双保险，避免最后一批帧一直占资源
    HAL_ETH_ReleaseTxPacket(&heth);            // 回收发完的TX描述符

#if ETH_DBG_FRAMES                            // 调试开关开了才编译心跳
    // 调试心跳，计数变化才打印，看中断、错误、内存池情况
    if ((gdbg_rx_irq != last_irq) || (gdbg_err_irq != last_err) || (gdbg_rd_fail != last_rdf))   // 三个计数有变化
    {                                          // 打印体开始
      printf("[CNT] rxirq=%lu errirq=%lu rdfail=%lu errcode=0x%08lX state=%u"   // 打各计数
             " pool: alloc=%lu null=%lu free=%lu\r\n",   // 打内存池计数
             (unsigned long)gdbg_rx_irq, (unsigned long)gdbg_err_irq,   // 收包和错误中断数
             (unsigned long)gdbg_rd_fail, (unsigned long)gdbg_err_code,   // 读失败数和错误码
             (unsigned)heth.gState,            // ETH状态
             (unsigned long)gdbg_pool_alloc, (unsigned long)gdbg_pool_null,   // 池分配数和空数
             (unsigned long)gdbg_pool_free);   // 池释放数
      last_irq = gdbg_rx_irq; last_err = gdbg_err_irq; last_rdf = gdbg_rd_fail;   // 更新记录值
    }                                          // 打印体结束
#endif                                         // 条件编译结束

    // 把所有积压帧读完，一次中断可能来多帧，信号量只通知有货，每帧交给tcpip内核
    while ((p = low_level_input(netif)) != NULL)   // 循环取帧直到取空
    {                                          // 取帧循环开始
#if ETH_DBG_FRAMES                             // 调试开关开了才编译打印
      // 调试打印，看收到帧的类型、长度、MAC，查ping和ARP问题用
      {                                        // 打印块开始
        static uint32_t rx_dbg_cnt = 0U;       // 已打印帧数
        if ((rx_dbg_cnt < 120U) && (p->len >= 14U))   // 只打前120个完整帧
        {                                      // 打印条件体开始
          const uint8_t *eth = (const uint8_t *)p->payload;   // 帧数据指针
          uint16_t etype = (uint16_t)(((uint16_t)eth[12] << 8) | eth[13]);   // 以太网类型
          printf("[RX] type=0x%04X len=%u dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X\r\n",   // 打印帧头
                 (unsigned)etype, (unsigned)p->len,   // 类型和长度
                 eth[0], eth[1], eth[2], eth[3], eth[4], eth[5],   // 目的MAC
                 eth[6], eth[7], eth[8], eth[9], eth[10], eth[11]);   // 源MAC

          rx_dbg_cnt++;                        // 计数加1
        }                                      // 打印条件体结束
      }                                        // 打印块结束
#endif                                         // 条件编译结束
      if (netif->input(p, netif) != ERR_OK)   // 交给内核失败，比如内存不够
        pbuf_free(p);                          // 必须自己释放，不能泄漏
    }                                          // 取帧循环结束

    // RBUS恢复安全网，DMA接收挂起标志，即使池扩大了也兜底，防止永久死锁
    if (gdbg_err_code & 0x0080U)  // DMA状态寄存器RBUS位
    {                                          // 恢复块开始
      gdbg_err_code = 0U;  // 清标志避免重复恢复

      // 第一步强制读数据，触发描述符更新，有空闲缓冲就补上、自动写接收轮询寄存器
      void *dummy_p = NULL;                    // 假读指针，数据不要
      HAL_ETH_ReadData(&heth, &dummy_p);       // 强制读触发描述符更新

      // 第二步手动写DMARPDR踢DMA，让它重新检查描述符环，写任意值都有效
      __DMB();  // 数据内存屏障，保证描述符写入对DMA可见
      WRITE_REG(heth.Instance->DMARPDR, 0U);   // 踢DMA重启接收
      gdbg_rbus_recover++;                      // 恢复计数加1
      printf("[ETH] RBUS recovery #%lu (DMARPDR kicked, pool: alloc=%lu null=%lu free=%lu)\r\n",   // 打印恢复日志
             (unsigned long)gdbg_rbus_recover,   // 第几次恢复
             (unsigned long)gdbg_pool_alloc, (unsigned long)gdbg_pool_null,   // 池分配和空次数
             (unsigned long)gdbg_pool_free);    // 池释放次数
    }                                          // 恢复块结束
  }                                            // 死循环结束
}                                              // 收包线程结束

// 函数：ethernetif_init —— 网卡初始化入口（netif_add回调）
// 功能：netif_add加网卡时LwIP自动调本函数：给网卡起名字（hw/sw）、把收包函数挂到netif的input指针、发包函数挂到linkoutput指针，再调low_level_init做真正的硬件初始化，是LwIP和硬件驱动的胶水层
// 参数：netif —— 正在被添加的网卡结构指针，收发函数指针和名字都挂在它上面
// 返回：err_t，ERR_OK代表网卡初始化完成；ERR_IF代表底层初始化失败，netif_add会失败返回
err_t ethernetif_init(struct netif *netif)     // 网卡初始化入口，netif_add回调
{
  LWIP_ASSERT("netif != NULL", (netif != NULL));   // 断言，参数不能空

#if LWIP_NETIF_HOSTNAME                       // 开了主机名功能才设置
  netif->hostname = "m2-gateway";                  // 主机名，DHCP上报用
#endif                                        // 条件编译结束

  netif->name[0] = IFNAME0;                        // 网卡名st，调试打印用
  netif->name[1] = IFNAME1;                        // 名字第二个字符't'

  netif->output     = etharp_output;               // 上层发包，先ARP找MAC再发
  netif->linkoutput = low_level_output;            // 真正发帧，零拷贝发送函数

  return low_level_init(netif);                    // 做底层硬件初始化
}                                                  // 入口函数结束

// 函数：pbuf_free_custom —— 自定义pbuf释放函数
// 功能：接收池pbuf的自定义释放器（挂在pbuf的custom_free指针上）：LwIP用完这帧要释放pbuf时自动调它，把pbuf连同DMA缓冲一起还回接收池给DMA继续写新帧用，形成接收缓冲循环
// 参数：p —— LwIP要释放的自定义pbuf指针
// 返回：void，无返回值
void pbuf_free_custom(struct pbuf *p)          // 自定义pbuf释放函数
{
  struct pbuf_custom *custom_pbuf = (struct pbuf_custom *)p;   // 转成自定义类型
  LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);   // 还回内存池，下次接收复用
  gdbg_pool_free++;                          // 调试计数，跟踪归还次数
}                                             // 释放函数结束

// 函数：HAL_ETH_RxAllocateCallback —— 接收缓冲分配回调
// 功能：HAL的ETH驱动缺接收缓冲时自动调：从接收池取一块缓冲给DMA直接写收到的数据——DMA不经过CPU直接往这块内存写包，是零拷贝接收的第一环
// 参数：buff —— 出参，分配到的缓冲区起始地址写到这里给HAL用
// 返回：void，无返回值，池空则给NULL由HAL丢帧
void HAL_ETH_RxAllocateCallback(uint8_t **buff)   // 接收缓冲分配回调
{
  struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);   // 从池里取一块
  gdbg_pool_alloc++;                                // 调试计数，分配次数
  if (p != NULL)                                    // 取到了
  {
    // 关键技巧offsetof，数据区地址反推结构体头地址，释放时要用到结构体头
    *buff = (uint8_t *)p + offsetof(RxBuff_t, buff);   // 给DMA数据区指针
    p->custom_free_function = pbuf_free_custom;     // 注册自定义释放函数
    // 构造自定义pbuf，PBUF_REF引用外部内存，不拷贝数据
    pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUF_SIZE);   // 构造零拷贝pbuf
  }
  else                                              // 池空了
  {
    *buff = NULL;   // 池空了，DMA拿不到缓冲，这就是RBUS死锁根因
    gdbg_pool_null++;                                // 调试计数，池耗尽次数
  }
}                                                   // 分配回调结束

// 函数：HAL_ETH_RxLinkCallback —— 接收链构建回调
// 功能：HAL收包过程中自动调：把DMA分多个缓冲写的帧串成pbuf链表——一帧太长会占多个接收缓冲，HAL每段调一次本函数，把新段链到pbuf链尾并累加总长度，最后拼成完整一帧的pbuf链
// 参数：pStart —— 出参，帧的pbuf链头指针；pEnd —— 出参，链尾指针（新段接在这里）；buff —— 本段数据缓冲地址；Length —— 本段数据字节数
// 返回：void，无返回值
void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)   // 接收链回调
{
  struct pbuf **ppStart = (struct pbuf **)pStart;   // 链表头二级指针
  struct pbuf **ppEnd   = (struct pbuf **)pEnd;    // 链表尾二级指针
  struct pbuf *p = NULL;                            // 当前段pbuf

  // 同样用offsetof反推pbuf头地址
  p = (struct pbuf *)(buff - offsetof(RxBuff_t, buff));   // 数据区地址减偏移得结构体头
  p->next = NULL;                                   // 新段next先置空
  p->tot_len = 0;      // 先清零，后面累加
  p->len = Length;     // 本段长度

  // 串链表，第一段当头，后面每段接在尾上
  if (*ppStart == NULL)                             // 链表还是空的
    *ppStart = p;                                   // 这段当链表头
  else                                              // 链表已有内容
    (*ppEnd)->next = p;                             // 接到链表尾
  *ppEnd = p;                                       // 更新链表尾指针

  // 累加整帧总长度，LwIP靠tot_len知道一帧多大
  for (p = *ppStart; p != NULL; p = p->next)        // 遍历整条链
  {
    p->tot_len += Length;                           // 每段都加上本段长度
  }
}                                                   // 接收链回调结束

// 函数：HAL_ETH_TxFreeCallback —— 发送释放回调
// 功能：DMA把帧发完后HAL自动调：把发送时引用的那个pbuf真正释放掉——发包时只是把pbuf指针给了DMA没释放，等DMA搬完内存发出去，在这里才free，保证pbuf生命周期覆盖整个发送过程
// 参数：buff —— 发送描述符里保存的pbuf指针转成的地址，即当时发包的那个pbuf
// 返回：void，无返回值
void HAL_ETH_TxFreeCallback(uint32_t *buff)         // 发送释放回调
{
  pbuf_free((struct pbuf *)buff);                   // 释放当时引用的pbuf
}

// 函数：HAL_ETH_RxCpltCallback —— 收包中断回调
// 功能：ETH接收中断里HAL自动调：中断服务程序里只做最轻的一件事——释放信号量唤醒收包线程，收包送协议栈的重活全留给线程做，最大化缩短中断占用时间，这是嵌入式标准的中断上下半件分离设计
// 参数：heth —— ETH外设句柄（当前未直接使用）
// 返回：void，无返回值
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)   // 收包中断回调
{
  (void)heth;                                       // 参数没用到，显式忽略
  gdbg_rx_irq++;                        // 收包中断计数
  osSemaphoreRelease(RxPktSemaphore);   // 释放信号量，唤醒收包线程
}

// 函数：HAL_ETH_TxCpltCallback —— 发送完成中断回调
// 功能：DMA发送完成中断里HAL自动调：本实现留空——发送描述符的回收在下次发包前统一做，TxFreeCallback已经负责pbuf释放，这里不需要额外动作
// 参数：heth —— ETH外设句柄（未使用）
// 返回：void，无返回值，空函数
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth)   // 发送完成回调
{
  (void)heth;                                       // 参数没用到，显式忽略
}

// 函数：HAL_ETH_ErrorCallback —— 错误中断回调
// 功能：ETH外设出错时中断里HAL自动调：只记录错误码和累计次数（轻量），不打日志不停机；RBUS这类非致命错误由收包线程的每秒兜底检查负责恢复，致命错误让看门狗体系兜底
// 参数：heth —— ETH外设句柄，从里面读当前错误码
// 返回：void，无返回值
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth)   // 错误中断回调
{
  gdbg_err_irq++;                                   // 错误中断计数
  gdbg_err_code = heth->DMAErrorCode;               // 记录DMA错误码
  // RBUS错误打印一次，避免高频错误刷屏
  if (gdbg_err_code & 0x0080U)  // DMA状态寄存器RBUS位
  {
    printf("[ETH-ERR] RBUS! pool_alloc=%lu null=%lu free=%lu\r\n",   // 打印RBUS
           (unsigned long)gdbg_pool_alloc, (unsigned long)gdbg_pool_null,   // 池分配和空数
           (unsigned long)gdbg_pool_free);    // 池释放数
  }
}                                              // 错误回调结束
