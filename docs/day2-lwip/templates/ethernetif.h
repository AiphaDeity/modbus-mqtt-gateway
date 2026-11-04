/**
 * @file    ethernetif.h
 * @brief   LwIP ↔ ETH 网卡接口（Day2 带 OS 版 · HAL 新架构零拷贝 + FreeRTOS）
 *
 * 【这份文件放哪】bsp/ethernet/ 目录下（与 ethernet.c 同目录）
 *
 * 【这个文件是干什么的】
 *   lwip 协议栈不认识硬件，它只认"网卡对象"（struct netif）。
 *   本文件就是"网卡对象和 ETH 硬件之间的翻译官"，负责两件事：
 *     1) ethernetif_init()：给 lwip 一个初始化回调（netif_add 时被调），
 *        实际干活的 low_level_init 在这里：配网卡参数 + 启动 ETH + 建收包线程
 *     2) lan8720_link_read()：让 ethernet.c 的链接线程能读 PHY 链接状态
 *
 * 【与裸跑版的区别】
 *   同事已把 FreeRTOS 接进工程（main.c 的 osKernelStart 接管主循环），
 *   所以收包由"信号量唤醒线程"完成（ethernetif_thread），不是主循环轮询。
 *   驱动收发机制仍是 HAL 新架构零拷贝（与 ST 官方 F429ZI LwIP 例程一致）。
 */
#ifndef __ETHERNETIF_H__
#define __ETHERNETIF_H__

/* lwip 的错误码类型（err_t）、网卡对象（struct netif）、IPv4 地址工具 */
#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LwIP 网卡初始化函数：作为 netif_add() 的初始化回调（ethernet.c 里注册）
   返回值：ERR_OK 成功 / ERR_IF 失败 */
err_t ethernetif_init(struct netif *netif);

/* ---- LAN8720A PHY 相关（给 ethernet.c 的链接线程调用）---- */
/* 读 PHY 链接状态：返回 1=已链接（网线插好），0=未链接。
   内部实现：读 PHY 基本状态寄存器 BSR 的 bit2（PHY_LINKED_STATUS） */
uint8_t lan8720_link_read(void);

#ifdef __cplusplus
}
#endif

#endif /* __ETHERNETIF_H__ */
