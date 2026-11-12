/**
 * @file    ethernet.h
 * @brief   以太网业务层接口（Day2 带 OS 版）
 *
 * 【这份文件放哪】bsp/ethernet/ 目录下
 *
 * 【接口约定】与 Day1 骨架保持一致（mqtt 服务以后直接调用）：
 *   m2_ethernet_init()      —— 上电调用一次：LwIP 初始化 + 起 DHCP/链接线程
 *   m2_ethernet_link_wait() —— 等网线/链路就绪（带超时）
 *   m2_ethernet_poll()      —— 带 OS 版为空实现（收包由线程完成，保留接口兼容）
 */
#ifndef __M2_ETHERNET_H__
#define __M2_ETHERNET_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回值（Day1 约定） */
typedef enum {
  ETH_OK       = 0,   /* 成功 */
  ETH_ERR_PHY  = 1,   /* PHY 初始化失败（MDIO 不通 / 不是 LAN8720） */
  ETH_ERR_LINK = 2,   /* 链路未建立（网线没插） */
  ETH_ERR_CONFIG = 3, /* 协议栈配置失败（内存不足等） */
} eth_result_t;

/* 启动任务串行调用；成功后幂等，失败重试复用已建资源；ETH_OK 不要求已插网线。 */
int32_t m2_ethernet_init(void);

/* timeout_ms 单位毫秒，0 表示无限等待；未初始化返回 ETH_ERR_CONFIG，超时返回 ETH_ERR_LINK。 */
int32_t m2_ethernet_link_wait(uint32_t timeout_ms);

/* 带 OS 版：收包由 ethernetif_thread 完成，此函数为空实现（保留接口） */
void m2_ethernet_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* __M2_ETHERNET_H__ */
