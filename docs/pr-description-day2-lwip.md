# PR 描述：feature/day2-lwip → main（M2 LwIP 评审修复）

> 用途：开 Gitee PR 时直接复制本文件内容贴进描述框。
> 链接：https://gitee.com/aiphadeity/modbus-mqtt-gateway/pull/new/aiphadeity:feature/day2-lwip...aiphadeity:main

---

## 改动目的

同事代码评审（合入 main 前）反馈的 4 项修复 + 1 项文档规范修订。**基线快照 `cf576d2` 是评审前已验证 PING 打通的版本**，本 PR 在此之上修改，便于对照。

## 基线与复测

- 评审基线：`cf576d2`（PING 打通原版代码）
- 本分支 HEAD：`cf3605b`
- 实测环境：PC `192.168.2.45` → 板子 `192.168.2.67`（DHCP），**ping 60/60，0% 丢包，avg 0ms**
- 板子 MAC（已加本地管理位）：`02:2d:00:26:e4:52`，PC 邻居表已确认
- 固件体积：FLASH 110112B/512KB（21.0%），RAM 113352B/128KB（86.48%）

## 合入前三项硬修复

### 1. LwIP 线程安全（`ethernet.c` / `lwipopts.h`）

- 原来在 link 检测线程和 DHCP 线程里直接调 `netif_set_up/down`、`netif_set_link_up/down`、`dhcp_start/stop`、`netif_set_addr` —— 这些函数在 `NO_SYS=0` 且 `LWIP_TCPIP_CORE_LOCKING=0` 下会和 tcpip_thread 竞争 netif 结构。
- 全部改走 `netifapi_*`（内部投递 mbox 给 tcpip_thread 执行）。
- **踩坑记录**：`src/api/netifapi.c` 整个文件被 `#if LWIP_NETIF_API` 包住，`lwipopts.h` 必须显式 `#define LWIP_NETIF_API 1`，否则链接报 undefined reference。已补。

### 2. TX 缓冲回收（`ethernetif.c`）

- **删除软件短帧补齐**：原 `tx_pad_buf/tx_pad_idx` 静态缓冲轮转与 DMA 描述符释放不绑定，存在"回收前 DMA 仍在读旧缓冲"的复用隐患。F4 MAC 硬件已配 `CRCPadCtrl=ETH_CRC_PAD_INSERT`（`eth.c:73`），短帧自动补 pad+FCS。
  - 验证：删完后 42 字节 ARP 短帧收发正常（PC ping 时 ARP 请求就是短帧），证明硬件补帧生效。
- **回收兜底**：收包线程原本每 1s 醒一次（信号量超时），现在每轮顺手 `HAL_ETH_ReleaseTxPacket()` —— 最后一批发完后即使没有下一次发包，pbuf 也能在 1s 内归还。不新建线程，TX 提交/回收/链路启停仍在同一任务上下文串行化。

### 3. 失败路径（`app_entry.c`）

- 原来网络初始化失败直接 `Error_Handler()`，局部网络故障升级为整机停机。
- 改为最多重试 5 次（间隔 2s），仍失败打日志后**继续装配** Modbus/存储任务，网络保持未就绪待后续恢复，不再把整机拖死。

### 4. MAC 规范（`ethernetif.c`）

- UID 派生 MAC 的前 3 字节不属于任何注册 OUI，按 IEEE 规范加本地管理位：`mac[0]=(mac[0]&0xFC)|0x02`，实测 `00:2D:..` → `02:2D:..`。
- `MACA0HR` bit31 注释更正为 F4 手册的 AE 位（地址表项有效）。
- 静态回落 IP 末字节由 UID 派生（原固定 `.30`，多板同网段会冲突）。

## 待办（不阻塞本 PR，但记录在案）

- 接 MQTT 前实测各任务栈高水位 / 最低剩余堆 / LwIP pbuf 池耗尽次数，据此调 TCP_MSS 和 pbuf 池大小。
- 发送中拔插网线的恢复收发专项实测（代码路径已通，未做拔线测试）。
- PHYAD0/nINTSEL 引脚事实请 M4 核对；多板同网段 MAC 冲突测试。

## 验证复现步骤

```bash
git checkout cf3605b
ninja -C firmware/app/build      # STM32CubeIDE 自带 ARM GCC + ninja
STM32_Programmer_CLI -c port=SWD -w firmware/app/build/modbus-mqtt-gateway.elf -v -rst
# 串口 115200 看 DHCP 分配到的 IP，PC ping 该 IP 60 次，应 0 丢包
```

详细原理、排障史和配置表见仓库根目录《M2网关_LwIP移植与PING打通_改动全记录.md》第十四章。
