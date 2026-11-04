# M2 工业网关 · Day2 LwIP 移植与 PING 打通 · 改动全记录

> **工程**：`modbus-mqtt-gateway`　**分支**：`feature/day2-lwip`
> **硬件**：STM32F407VET6（天空星核心板）+ 底板 LAN8720A（RMII）
> **软件**：FreeRTOS（CMSIS-RTOS2）+ LwIP 2.x（带 OS 版，`NO_SYS=0`）
> **时间跨度**：2026-11-03（移植落地） → 2026-11-04（排障直至 PING 打通）
> **文档性质**：给项目组其他成员的**完整改动交接记录**（含实现原理、踩坑过程、验证数据、可复用调试方法）
> **最终结论**：`ping 192.168.2.4` → **60 发 60 收、0% 丢包、平均 1 ms**；PC ARP 表 `192.168.2.4 → 00-2D-00-26-E4-52 Reachable`

---

## 目录

| 章节 | 内容 | 谁该看 |
|---|---|---|
| 一 | 背景、目标与验收标准 | 全员 |
| 二 | 硬件与运行环境（引脚/时钟/工具链） | 全员 |
| 三 | 软件架构与线程模型（改完后长什么样） | 全员，**新人必读** |
| 四 | 新增文件清单（11 个文件逐个说明） | 全员 |
| 五 | 已有文件改动清单（7 个文件逐条 diff） | 全员 |
| 六 | 关键实现原理（零拷贝、信号量桥接、临界区约束、时基分工） | 嵌入式/驱动同学 |
| 七 | 排障历程：三个致命问题（现象→证据→定位→修复） | **全员必读** |
| 八 | 被推翻的假说清单（别再重复踩） | 全员 |
| 九 | 调试手段与命令速查（pyOCD / HOTPLUG / 探针 / PC 侧取证） | 排障同学 |
| 十 | 最终验证结果（实测数据） | 全员 |
| 十一 | 关键配置参数汇总表 | 全员 |
| 十二 | 遗留问题与下一步计划 | 项目负责人 |
| 十三 | 如何复现与注意事项（编译/烧录/验证） | 接手同学 |
| 附录 A | 完整启动串口日志示例 | — |
| 附录 B | 文件-代码位置索引 | — |

---

## 一、背景、目标与验收标准

### 1.1 背景

M2 工业网关是一块自研板（STM32F407VET6 + LAN8720A 百兆 PHY），软件按需求分为多天任务推进：

| 阶段 | 内容 | 状态 |
|---|---|---|
| Day1 | 模组目录骨架 + 串口/RS485 等基础外设 + MQTT 消息正反例 | 已合入 `origin/main` |
| **Day2（本文档）** | **以太网 + LwIP 协议栈移植，做到电脑能 PING 通板子** | **已完成** |
| Day3+ | Modbus 主机/从机、MQTT 上云、业务编排 | 待开发 |

Day2 的移植路线是「**抄正点原子探索者 F407 的 LwIP 带 OS 例程，按天空星板的硬件差异改写**」，而不是从零写协议栈驱动。方案演进过一次：

| 时间点 | 方案 | 原因 |
|---|---|---|
| 原计划 | 裸跑 `NO_SYS=1`，主循环轮询收包 | 当时以为工程还没接 FreeRTOS |
| 实际采用 | 带 OS `NO_SYS=0` + 例程 `sys_arch.c` | 同事重构后的工程已把 FreeRTOS 接进 `main.c`（`osKernelStart()` 接管主循环），裸跑轮询无处安放；例程本身就是带 OS 工程，材料现成 |

### 1.2 目标

1. 板子上电、插网线后，串口能打印出 IP（DHCP 成功或超时回落静态 IP）；
2. 电脑能 `ping` 通板子，且长时间稳定；
3. 收发包链路打通，为后续 TCP（MQTT）/UDP 业务铺路。

### 1.3 验收标准（实测已达成）

| 项 | 标准 | 实测 |
|---|---|---|
| PING 连通性 | 0% 丢包 | ✅ 60 发 60 收，0% 丢包 |
| PING 时延 | < 10 ms（百兆局域网） | ✅ min 1 ms / max 3 ms / avg 1 ms |
| ARP 学习 | PC 邻居表能学到板子 MAC | ✅ `00-2D-00-26-E4-52 Reachable` |
| 长时间稳定性 | 无死机 / 无 hook 触发 | ✅ 连续运行 + 长 ping 均正常 |
| 发送通路 | 既能发广播也能发单播 | ✅ DHCP 广播 + ARP 单播应答 + ICMP 回包全部正常 |

---

## 二、硬件与运行环境

### 2.1 硬件事实（已从原理图确认，勿再怀疑）

| 项 | 值 |
|---|---|
| 主控 | 天空星核心板 = STM32F407VET6（8 MHz HSE → PLL → 168 MHz） |
| PHY | 底板 U17 = **LAN8720A**（RMII 百兆） |
| PHY 时钟 | X1 = 25 MHz 晶振 → LAN8720A → CLKOUT 输出 **50 MHz** → 回灌 PA1（RMII REF_CLK） |
| RJ45 | J1 = HR911105A（带网络变压器） |
| PHY 复位 | **ETH_NRST（低有效）→ PE12**（上电由 `gpio.c` 拉高即运行态） |
| PHY 地址 | `LAN8720_PHY_ADDRESS = 0x00`（由 nINTSEL 引脚决定） |
| PHY 识别值 | IDR1 = `0x0007`、IDR2 = `0xC0F1` |

RMII 引脚映射（天空星板，与探索者例程的 G 口不同，**同事的 CubeMX 工程已按本板重新生成，无需改动**）：

| 信号 | 引脚 | 信号 | 引脚 |
|---|---|---|---|
| ETH_REF_CLK | PA1 | ETH_TX_EN | PB11 |
| ETH_MDIO | PA2 | ETH_TXD0 | PB12 |
| ETH_CRS_DV | PA7 | ETH_TXD1 | PB13 |
| ETH_MDC | PC1 | ETH_RXD0 | PC4 |
| — | — | ETH_RXD1 | PC5 |

### 2.2 软件运行环境

| 项 | 值 |
|---|---|
| 编译 | `ninja -C firmware/app/build`（CMake + Ninja，ARM GCC 14.3.1） |
| 烧录 | ST-LINK + `STM32_Programmer_CLI`（SWD，`mode=UR` 复位烧录 / `mode=HOTPLUG` 不停机读现场） |
| 串口 | **COM6 / 115200**（CH340；核心板丝印 `U1F` 实为 `U1T`，即 USART1 TX = PA9） |
| 调试器 | pyOCD 0.46.1（`python -m pyocd commander -t cortex_m`）—— 本机唯一可用的**停核**通道 |
| 目标网络 | 办公室交换机网段 `192.168.2.0/24`，网关 / DHCP 服务器 = `192.168.2.1` |
| 板子地址 | DHCP 分配 **192.168.2.4 / 255.255.255.0 / gw 192.168.2.1** |
| 板子 MAC | **00:2D:00:26:E4:52**（由芯片 UID 生成，见 6.1） |
| PC | 网卡名「以太网」，`192.168.2.45/24`，MAC `A8:2B:DD:60:08:16`，1 Gbps |

### 2.3 编译产物占用（最终干净版本，`ninja` 构建末尾实测）

```
Memory region         Used Size  Region Size  %age Used
             RAM:      113600 B       128 KB     86.67%
          CCMRAM:          0 GB        64 KB      0.00%
           FLASH:      109572 B       512 KB     20.90%
```

交叉核对（`arm-none-eabi-size`，不含段对齐填充，故略小）：

```
text       data      bss        dec        hex
109320      244    113352     222916      366c4   modbus-mqtt-gateway.elf
```

> ⚠️ RAM 已到 86.67%，后续再加功能（Modbus 缓冲、MQTT 报文）前，**先核对 FreeRTOS heap 余量和各线程栈高水位**，否则很容易重演"栈溢出→整机停机"的老问题（见第七章问题 2）。

---

## 三、软件架构与线程模型

### 3.1 分层

```
┌────────────────────────────────────────────────────────────┐
│ 应用层   integration/app_entry.c  →  App_Entry() 任务       │
│          （调 m2_ethernet_init()，后续挂 Modbus/MQTT 业务） │
├────────────────────────────────────────────────────────────┤
│ BSP 层   bsp/ethernet/ethernet.c       业务/接入（netif+DHCP+链路）│
│          bsp/ethernet/ethernetif.c    网卡驱动（零拷贝收发/PHY）  │
│          bsp/ethernet/sys_arch.c      LwIP↔FreeRTOS 适配（搬运）  │
│          bsp/ethernet/lwipopts.h      LwIP 裁剪配置              │
├────────────────────────────────────────────────────────────┤
│ 协议栈   Middlewares/LwIP/             lwip 源码（124 个 .c）     │
├────────────────────────────────────────────────────────────┤
│ 驱动层   Core/Src/eth.c（CubeMX 生成）  HAL_ETH_Init / MSP / 中断 │
│          Drivers/STM32F4xx_HAL_Driver  HAL 驱动                  │
├────────────────────────────────────────────────────────────┤
│ 内核     FreeRTOS（CMSIS-RTOS2）+ HAL 时基 TIM6                  │
└────────────────────────────────────────────────────────────┘
```

**分工边界（重要，别重复定义）**：

- `Core/Src/eth.c`（CubeMX 生成）：`heth`、`DMARxDscrTab[4]`、`DMATxDscrTab[4]`、`TxConfig` 全局对象 + `MX_ETH_Init()` + `HAL_ETH_MspInit()`（GPIO/时钟/中断）。
- `bsp/ethernet/ethernetif.c`：**只**补 LwIP 需要的部分——零拷贝收发、收包线程、LAN8720 探测/软复位/链接检测；**不写** `HAL_ETH_MspInit`（会重复定义）。
- `Core/Src/main.c`：`__io_putchar` 重定向已由同事写好，**不要**再写。

### 3.2 线程模型（改完后的真实状态）

| 线程名 | 入口 | 创建者 | 优先级 | 栈 | 职责 |
|---|---|---|---|---|---|
| `lwip_thread` | `tcpip_thread` | `tcpip_init()` | `configMAX_PRIORITIES-2` = 54 | **1280 字 = 5 KB** | 协议栈内核：ARP/IP/ICMP/UDP/TCP/DHCP 报文处理 |
| `ethif` | `ethernetif_thread` | `low_level_init()` | `osPriorityHigh`(40) | 1024 字节 | 等信号量 → `HAL_ETH_ReadData` 取帧 → `netif->input` 交给内核 |
| `eth_dhcp` | `lwip_dhcp_process` | `m2_ethernet_init()` | 4 | **128*6 字 = 3 KB** | DHCP 状态机（申请/等待/成功/超时回落静态） |
| `eth_link` | `lwip_link_thread` | `m2_ethernet_init()` | 3 | **128*6 字 = 3 KB** | 每秒轮询 PHY 链接状态，插拔时启停 ETH + 通知 DHCP |
| `defaultTask`（调 `App_Entry`） | `StartDefaultTask` → `App_Entry` | `MX_FREERTOS_Init()` | `osPriorityNormal`(24) | 静态缓冲 512 字 = 2 KB | 应用入口：**在这里调 `m2_ethernet_init()`**，后续挂 Modbus/MQTT 业务 |

**栈单位陷阱（务必分清，这是问题 2 的根源）**：

- `sys_arch.c` 里 `LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS = 1` → `lwipopts.h` 的 `TCPIP_THREAD_STACKSIZE`、`ethernet.c` 里传给 `sys_thread_new` 的栈参数，单位是**字（4 字节）**，不是字节。
- CMSIS-RTOS2 的 `osThreadAttr_t.stack_size` 单位是**字节**（`ethernetif_thread` 的 1024 就是 1 KB）。

### 3.3 收包数据流（零拷贝）

```
网线 → PHY → MAC/DMA
   ↓ 每收一帧：DMA 把数据直接写进 RX_POOL 里的 pbuf 数据区（不经过 CPU 拷贝）
ETH 接收中断 → HAL_ETH_IRQHandler → HAL_ETH_RxCpltCallback
   ↓ 只做一件事：osSemaphoreRelease(RxPktSemaphore)
ethernetif_thread（ethif） 醒来
   ↓ while (HAL_ETH_ReadData(&heth, &p) == HAL_OK)
   ↓    HAL 内部：把该描述符的 buf 串成 pbuf（RxLinkCallback），并把描述符重新挂上新 buf（RxAllocateCallback）
   ↓ netif->input(p, netif) = tcpip_input → 投递到 lwip_thread 的 mbox
tcpip_thread：解析 ARP / ICMP / UDP / DHCP ……
```

### 3.4 发包数据流（零拷贝）

```
tcpip_thread 组包（pbuf）→ netif->output = etharp_output（查 ARP 表，必要时先发 ARP 请求）
   ↓ netif->linkoutput = low_level_output
   ↓   ★先 HAL_ETH_ReleaseTxPacket() 回收已发完的描述符（问题 3 的修复点）
   ↓   把 pbuf 链的 payload 指针直接填进 ETH_BufferTypeDef 数组（不拷贝）
   ↓   ★不足 60 字节的短帧(如 ARP 42 字节)拷进静态缓冲补 0（双保险，见 8.1）
   ↓ HAL_ETH_Transmit_IT() → DMA 把帧送上线
发送完成中断 → HAL_ETH_TxCpltCallback（空实现）
   ↓ 描述符与 pbuf 的释放在下一次 low_level_output 开头由 ReleaseTxPacket 完成
```

---

## 四、新增文件清单

> 全部位于 `firmware/app/`，除特别说明外均为本次新增（当前 git 状态为 untracked，待提交）。

| # | 文件 | 行数/大小 | 来源与职责 |
|---|---|---|---|
| 1 | `bsp/ethernet/ethernet.c` | 22.6 KB | **新写**（逻辑照例程 `netconf.c`）。LwIP 接入层：`tcpip_init` → `netif_add` → 链接线程 + DHCP 线程状态机 + 链路等待接口。带超详细中文注释 |
| 2 | `bsp/ethernet/ethernet.h` | 1.4 KB | 新写。对外接口：`m2_ethernet_init()` / `m2_ethernet_link_wait()` / `m2_ethernet_poll()` / `ETH_OK`、`ETH_ERR_LINK`、`ETH_ERR_CONFIG` 等返回码 |
| 3 | `bsp/ethernet/ethernetif.c` | 37.3 KB | **新写**（抄 ST 官方 F429ZI LwIP 例程的 HAL 零拷贝架构 + 探索者的 LAN8720 寄存器操作）。网卡驱动：`low_level_init/output/input` + 零拷贝回调族 + LAN8720 探测/软复位/自动协商/链接读取 |
| 4 | `bsp/ethernet/ethernetif.h` | 1.8 KB | 新写。`ethernetif_init` 声明、`lan8720_link_read` 声明 |
| 5 | `bsp/ethernet/lwipopts.h` | 8.8 KB | **新写**（照例程原值 + 本工程必要调参）。带 OS 版配置：`NO_SYS=0`、线程/邮箱尺寸、内存池、DHCP/ICMP/ARP、`MEM_SIZE=16K` 等 |
| 6 | `bsp/ethernet/sys_arch.c` | 15.5 KB | **原样搬运**（探索者例程 `lwip-2.2.0/arch/sys_arch.c`），一行未改。LwIP ↔ FreeRTOS 适配层：`sys_sem/sys_mbox/sys_thread/sys_now/sys_arch_protect` |
| 7 | `bsp/ethernet/CMakeLists.txt` | 650 B | 新写。`m2_ethernet` 静态库（含上述 3 个 .c），并 `PUBLIC` 链接 `stm32cubemx` + `lwip` |
| 8 | `Middlewares/LwIP/` | 124 个 `.c` | **整包引入** lwip 源码 + `CMakeLists.txt`（`lwip` 库）。注意 `src/include/lwip/arch/` 下的 `cc.h`/`sys_arch.h`/`cpu.h`/`perf.h` 是从例程拷入的**编译器/OS 特性头**，lwip 源码不自带，**勿删** |
| 9 | `docs/day2-lwip/README.md` + `templates/` | 6.1 KB | 移植指南 + 手敲模板（`templates/ethernet.c`、`ethernetif.c`、`lwipopts.h`、CMakeLists diff、main.c 改动说明） |
| 10 | `LWIP_PING排障记录_修正版.md` | 见文档 | 排障权威记录（第九章为最终结论） |
| 11 | `LWIP_PING_状态快照_重启恢复.md` | 见文档 | 快速恢复上下文用（已更新为"PING 已通"） |

> `firmware/app/build/` 是本机构建目录（untracked），**不要提交**，也不要删除（同事约定：改 `.h` 后用 touch 触发重编，而不是 clean）。

---

## 五、已有文件改动清单

> 以下是 git 已跟踪文件的真实 diff（`git diff` 实测，共 7 个文件、+73/−6 行）。逐条说明「改了什么 / 为什么」。

### 5.1 `firmware/app/CMakeLists.txt`（+3）

```diff
 add_subdirectory(cmake/stm32cubemx)
+add_subdirectory(Middlewares/LwIP)
 add_subdirectory(integration)
 add_subdirectory(bsp)
@@ target_link_libraries
     stm32cubemx
+    lwip
+    m2_ethernet
```

**为什么**：把 lwip 协议栈编进来；把 `m2_ethernet` 库链接到可执行文件。否则 `lwip/opt.h` 找不到、`sys_sem_new` 等一堆 undefined reference。

### 5.2 `firmware/app/bsp/CMakeLists.txt`（+3）

```diff
+# Day2: 以太网 BSP 子模块（LAN8720A + LwIP 移植层）
+add_subdirectory(ethernet)
```

**为什么**：让 `bsp/ethernet` 参与构建。

### 5.3 `firmware/app/integration/app_entry.c`（+13）

```diff
+#include "ethernet.h"
 void App_Entry(void *argument)
 {
+  /* 网卡初始化必须在【任务上下文】里做，不能放回 main.c 的 osKernelStart 之前 */
+  if (m2_ethernet_init() != ETH_OK)
+  {
+    Error_Handler();
+    return;
+  }
```

**为什么**：这是**问题 1 的最终修复落点**。`m2_ethernet_init()` 内部会建线程、调 `netif_add`、建信号量，这些在调度器启动前做会让 FreeRTOS 的 `uxCriticalNesting` 配不平 → `BASEPRI` 锁死 0x50 → 中断全屏蔽。移到 `App_Entry` 任务里，此时 `uxCriticalNesting = 0`，临界区能正确恢复。

### 5.4 `firmware/app/Core/Src/main.c`（+20/−3）

```diff
 #include "ethernet.h"
   /* USER CODE BEGIN 2 */
+  setvbuf(stdout, NULL, _IONBF, 0);      /* ★stdout 改无缓冲，printf 不再被吞 */
   printf("Gateway boot\r\n");
-  fflush(stdout);
-
+  osKernelInitialize();                  /* ★必须先于任何 os 对象创建 */
+  /* ★★网卡初始化【不能】放在这里！（已移到 app_entry.c，原因见注释） */
   /* USER CODE END 2 */
   /* Init scheduler */
-  osKernelInitialize();
   MX_FREERTOS_Init();
   osKernelStart();
```

**为什么（3 个独立修复）**：

1. **`setvbuf(stdout, NULL, _IONBF, 0)`**：Picolibc 的 `stdout` 默认**全缓冲**，`printf` 内容攒在缓冲里，缓冲区不满就不调 `_write`（串口）。现象是只有带 `fflush` 的 `"Gateway boot"` 能显示，`ethernet.c` 里 `"DHCP assigned"`、`"Static IP"` 等全部被吞，**极易误判成"程序卡死/没走到那一步"**。
2. **`osKernelInitialize()` 前移**：必须早于任何 RTOS 对象创建（`MX_FREERTOS_Init` 会检查内核状态），否则信号量/线程静默失败。
3. **网卡初始化搬走**：见 5.3。

### 5.5 `firmware/app/Core/Inc/FreeRTOSConfig.h`（+12/−2）

```diff
-#define configTOTAL_HEAP_SIZE  ((size_t)15360)
+#define configTOTAL_HEAP_SIZE  ((size_t)18432)

-#define configASSERT( x ) if ((x) == 0) {taskDISABLE_INTERRUPTS(); for( ;; );}
+void vAssertCalled(const char *pcFile, unsigned long ulLine);
+#define configASSERT( x ) if ((x) == 0) { vAssertCalled(__FILE__, __LINE__); }

 #define USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION 0   /* 保持 0，并加了长注释说明时基分工 */
```

**为什么**：

- `configTOTAL_HEAP_SIZE` 15360 → **18432**：多出来的线程栈（link 3 KB、dhcp 3 KB、tcpip 5 KB、收包 1 KB）全部从 FreeRTOS heap 分配。原 15 KB 装不下，`xTaskCreate` 会失败或直接 `vApplicationMallocFailedHook`。**这是"栈溢出"问题的配套修复**。
- `configASSERT` 改造：原实现失败就 `taskDISABLE_INTERRUPTS(); for(;;);`，现场一片漆黑看不出是哪个断言。改成 `vAssertCalled(__FILE__, __LINE__)`，串口直接打印断言位置。
- 加了 `USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION = 0` 的**长注释**：这是历史踩坑点（曾误改成 1 并自写 `SysTick_Handler` 调 `HAL_IncTick()`，导致 `uwTick` 被 TIM6 + SysTick 双份递增，所有 HAL 超时失真）。**不要再动它**。

### 5.6 `firmware/app/Core/Src/freertos.c`（+17/−1）

```diff
+#include <stdio.h>
 void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
 {
+  printf("\r\n[HOOK] STACK OVERFLOW task=%s\r\n", pcTaskName ? (char*)pcTaskName : "?");
   Error_Handler();
 }
 void vApplicationMallocFailedHook(void)
 {
+  printf("\r\n[HOOK] MALLOC FAILED freeHeap=%u\r\n", (unsigned)xPortGetFreeHeapSize());
   Error_Handler();
 }
+void vAssertCalled(const char *pcFile, unsigned long ulLine)
+{
+  __disable_irq();
+  printf("\r\n[ASSERT] %s:%lu\r\n", pcFile, ulLine);
+  for (;;) {}
+}
```

**为什么**：原来的 hook 直接进 `Error_Handler()`（里面是 `__disable_irq()` + 死循环），**什么线索都不留**。改造后：
- 爆栈会打印**是哪个任务**（问题 2 就是靠这行锁定的）；
- 堆耗尽会打印**剩余 heap**；
- 断言会打印**文件:行号**。

> 这 3 个 hook 是长期保留的"黑匣子"，**建议不要删**。

### 5.7 `firmware/app/Core/Src/stm32f4xx_it.c`（+9）

```diff
+#include "FreeRTOS.h"
+#include "task.h"
 /* USER CODE BEGIN 1 */
+/* ★这里不要写 SysTick_Handler！
+   SysTick_Handler 由 cmsis_os2.c 的默认实现提供（只喂 FreeRTOS 心跳），
+   HAL 时基由 TIM6 单独提供（stm32f4xx_hal_timebase_tim.c）。
+   若在此处再定义并调 HAL_IncTick()，uwTick 会被双份递增，HAL 超时全部失真。 */
 /* USER CODE END 1 */
```

**为什么**：只留了**警示注释**（历史踩坑），没有功能改动。曾经在这里写过自定义 `SysTick_Handler`，已回退。

---

## 六、关键实现原理（新人必读）

### 6.1 MAC 地址：由芯片 UID 生成，两处必须同步

```c
static void get_stm32_mac(uint8_t *mac);      /* ethernetif.c */

uint32_t uid[3] = { *(uint32_t*)0x1FFF7A10, *(uint32_t*)0x1FFF7A14, *(uint32_t*)0x1FFF7A18 };
/* mac[0..3] = uid[0] 的 4 个字节；mac[4] = 12 字节累加和；mac[5] = 12 字节异或和 */
```

**为什么要 UID 生成**：同事 `eth.c` 里是固定 MAC `00:80:E1:00:00:00`，多块板子接同一网段会 MAC 冲突（交换机 CAM 表来回翻）。用 UID 保证每块板唯一。

**关键：两处必须写成同一个值**，否则"ARP 报文里报一个地址、实际发帧是另一个"，直接断网：

| 位置 | 写入方式 |
|---|---|
| `netif->hwaddr`（LwIP 侧，ARP/DHCP 报文里的源 MAC） | `memcpy(netif->hwaddr, mac, 6)` |
| MAC 寄存器（HAL 侧，DMA 实际发出的帧的源 MAC） | `ETH->MACA0LR = mac[0..3]`、`ETH->MACA0HR = mac[4..5] | 0x80000000U`（bit31 = 源地址有效位，**必须置 1**） |

> F4 HAL 1.28 **没有** `HAL_ETH_SetMACAddress()`（那是 F7/H7 的 API），只能直接操作寄存器。本机实测 MAC = `00:2D:00:26:E4:52`（`MACA0HR = 0x800052E4`、`MACA0LR = 0x26002D00`）。

### 6.2 零拷贝接收：`offsetof` 反推结构体头

```c
typedef struct {
  struct pbuf_custom pbuf_custom;                            /* lwIP 自定义 pbuf 头 */
  uint8_t buff[(ETH_RX_BUF_SIZE + 31) & ~31] __ALIGNED(32);  /* 32 字节对齐数据区 */
} RxBuff_t;

LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT /*10*/, sizeof(RxBuff_t), "...");
```

- DMA 描述符要的是 `buff` 的地址；`pbuf_custom` 是 lwIP 的 pbuf 头。
- 回收时用 `buff - offsetof(RxBuff_t, buff)` 反推回结构体头，交给 `LWIP_MEMPOOL_FREE`。
- 池大小 ≥ DMA Rx 描述符数（`ETH_RX_DESC_CNT = 4`），**本工程给 10 个留余量**。

### 6.3 三个 HAL 回调族的职责（**别写错**）

| 回调 | 谁调 | 本工程实现 |
|---|---|---|
| `HAL_ETH_RxAllocateCallback(uint8_t **buff)` | HAL 在需要给描述符挂新缓冲时 | 从 `RX_POOL` 取一块，`pbuf_alloced_custom` 包成 pbuf，`*buff` 指向数据区 |
| `HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)` | HAL 收到一帧、把各段串成 pbuf 链时 | 反推 pbuf 头、`p->len = Length`、串链、累加 `tot_len` |
| `HAL_ETH_TxFreeCallback(uint32_t *buff)` | **`HAL_ETH_ReleaseTxPacket()` 内部**，参数是 `TxConfig.pData`（我们存的 pbuf） | `pbuf_free((struct pbuf *)buff)` |
| `HAL_ETH_RxCpltCallback(ETH_HandleTypeDef*)` | ETH 中断（收到帧） | **只** `osSemaphoreRelease(RxPktSemaphore)`（+调试计数） |
| `HAL_ETH_TxCpltCallback(ETH_HandleTypeDef*)` | ETH 中断（发送完成） | **空**。描述符回收放在 `low_level_output` 里做（见问题 3） |
| `HAL_ETH_ErrorCallback(ETH_HandleTypeDef*)` | ETH 中断（DMA 错误） | 记 `gdbg_err_irq` / `gdbg_err_code = heth->DMAErrorCode` |

### 6.4 中断 → 线程桥接：为什么用二值信号量

- 中断里**只做** `osSemaphoreRelease()`，不碰协议栈、不 memcpy。
- 收包线程 `osSemaphoreAcquire(RxPktSemaphore, 1000U)` 醒来后，`while (low_level_input() != NULL)` 把积压帧**全部读完**（中断可能一次来多帧，信号量只是"有货"通知）。
- 超时给 1 秒（不永久等待）：调试时每秒能打 `[CNT]` 心跳；正式运行时醒来读一次没包就立刻睡回去，无副作用。

### 6.5 临界区 / 中断优先级约束（PING 不通的第一层原因）

| 概念 | 本工程值 |
|---|---|
| `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` | 5 → `configMAX_SYSCALL_INTERRUPT_PRIORITY = 0x50` |
| FreeRTOS 临界区效果 | `BASEPRI = 0x50`，**屏蔽所有优先级数值 ≥ 5 的中断** |
| ETH 中断优先级 | `HAL_NVIC_SetPriority(ETH_IRQn, 5, 0)`（`eth.c`） |
| HAL 时基 TIM6 优先级 | `TICK_INT_PRIORITY = 15` → 优先级字段 `0xF0`，**同样 ≥ 0x50，照屏蔽** |

**结论（血泪教训）**：临界区里**绝对不能**放依赖 `HAL_Delay`/`HAL_GetTick` 的代码。`HAL_Delay` 等的是 `uwTick` 递增，而 `uwTick` 靠 TIM6 中断，TIM6 优先级 15 ≥ 5 → 在临界区里 TIM6 进不来 → `uwTick` 冻结 → `HAL_Delay` 里的 `while` 永远退不出去 → **死循环**。

> 早期文档曾写"优先级 0xF0 不受 0x50 屏蔽"，**这是错的**。ARM 语义：BASEPRI 屏蔽的是优先级**数值 ≥ BASEPRI**的中断，0xF0 > 0x50，照样被屏蔽。已实测推翻。

### 6.6 时基分工：HAL 用 TIM6，FreeRTOS 用 SysTick

| 用途 | 来源 | 说明 |
|---|---|---|
| `HAL_GetTick() / HAL_Delay()` | **TIM6**（`stm32f4xx_hal_timebase_tim.c`） | `TICK_INT_PRIORITY = 15`，中断里 `HAL_IncTick()` |
| FreeRTOS tick | **SysTick**（`cmsis_os2.c` 的默认 `SysTick_Handler`） | 喂 `xPortSysTickHandler()` |

**两者互不干扰**，`USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION` 必须保持 **0**。
> 曾在 `stm32f4xx_it.c` 自写 `SysTick_Handler` 并调 `HAL_IncTick()` → `uwTick` 双份递增 → 所有 HAL 超时失真。已回退。

### 6.7 DHCP 状态机与静态回落

```
        ┌──────────┐  netif up 回调     ┌───────────┐   dhcp_supplied_address()  ┌──────────────────┐
        │  OFF(0)  │ ───────────────→  │ START(1)  │ ─────────────────────────→ │ ADDRESS_ASSIGNED │
        └──────────┘                   └─────┬─────┘                            │      (3) 稳定态   │
              ▲                              │ dhcp_start()                     └──────────────────┘
              │                              ▼
              │                        ┌──────────────┐  tries > 10（LWIP_MAX_DHCP_TRIES）
              │                        │ WAIT_ADDRESS │ ─────────────────────→ TIMEOUT(5)
              │                        │     (2)      │                        停 DHCP + 写静态 IP
              │                        └──────────────┘                        192.168.2.30/24 gw .1
              │  LINK_DOWN(4) 网线断：dhcp_stop() → OFF
```

- DHCP 线程优先级 4、每秒跑一次状态机（DHCP 响应本身是秒级）。
- 链接线程优先级 3、每秒轮询 PHY（RMII 没有"链接中断"引脚给 MCU，只能轮询），带**连续 2 次断线才当真**的去抖。
- 静态回落值：`192.168.2.30 / 255.255.255.0 / gw 192.168.2.1`（`lwip_comm_default_ip_set`）。

### 6.8 printf 必须无缓冲

```c
setvbuf(stdout, NULL, _IONBF, 0);   /* main.c USER CODE 2 区，printf 之前 */
```

Picolibc 的 `stdout` 默认全缓冲。不设无缓冲，你会看到"程序明明跑到了却没有任何打印"，这是**最容易被误判成死机**的现象之一（本项目就误判过一整轮）。

---

## 七、排障历程：三个致命问题

> 时间线：11-03 移植完成、编译通过、烧录后**串口停在 `[ETH] sem=...`**；11-04 逐层定位到三个独立缺陷，最终 PING 通。
> 这一章是给同事**最有价值**的部分——三个人人都会踩的坑。

### 问题 1：初始化位置错了 + 临界区里夹 `HAL_Delay`

#### 现象
串口打印到：

```
[ETH] sem=0x20002fe0
        ← 到此为止，"Start_IT done" 永不出现
```

#### 证据链

| 步骤 | 手段 | 结果 |
|---|---|---|
| 1 | 串口加诊断打印（`Start_IT` 前打 `PRIMASK/BASEPRI/uwTick`） | `BASEPRI=0x50`，`uwTick` 不再增长 |
| 2 | pyOCD 停核读 `ISPR1 @0xE000E204` | 读到 `0x00400000`（TIM6 挂起但从不被处理；另一次现场读到 `0x20400000`，TIM6 与 ETH **同时**挂起） |
| 3 | 读抢占优先级寄存器 `IPR13 @0xE000E434` 的优先级字段 | TIM6 = `0xF0`（15），数值 ≥ BASEPRI `0x50` → **被屏蔽** |
| 4 | 两次读 `uwTick @0x20000114`（间隔数秒） | **同一个值、完全不动** → 时基冻结 |
| 5 | FreeRTOS `port.c` 加临界区插桩（`g_dbg_crit_nest`/`lr[]`） | 稳定运行阶段 `nest = 0`（**并不在临界区里**）→ 说明"停在临界区里出不来"的猜测不成立，死循环在 `Error_Handler`（属问题 2） |

#### 定位

`m2_ethernet_init()` 被放在 `main.c` 的 `osKernelStart()` **之前**，而 `ethernet.c` 里又用 `taskENTER_CRITICAL()/taskEXIT_CRITICAL()` 包着 `netif_add`：

- 调度器未启动时，`port.c` 的 `uxCriticalNesting` 初始值是**哨兵值 `0xAAAAAAAA`**；
- 一次 `enter`(++)/`exit`(--) 配平后，它依然 ≠ 0 → `vPortExitCritical` 里的 `if (uxCriticalNesting == 0) portENABLE_INTERRUPTS()` 永远不成立 → **BASEPRI 永久停在 0x50**；
- 于是 TIM6（优先级 15）和 ETH（优先级 5）全部被屏蔽；
- `netif_add → ethernetif_init → low_level_init` 里有 PHY 软复位/自动协商的 `HAL_Delay`，以及 `HAL_ETH_Start_IT` 内部的 `HAL_Delay(ETH_REG_WRITE_DELAY)` → **等不到 `uwTick` 递增 → 死循环**。

#### 修复（两处）

1. **删掉 `netif_add` 外层的 `taskENTER_CRITICAL/EXIT`**（`ethernet.c`）：LwIP 内部有自己的 `sys_arch_protect` 保护，外层再包一层毫无收益，只会长时间关中断。
2. **`m2_ethernet_init()` 移到任务上下文**（`integration/app_entry.c` 的 `App_Entry`），同时把 `osKernelInitialize()` 前移到 `MX_FREERTOS_Init()` 之前。

#### 验证
`Start_IT done` 打印出来，链路 UP，串口继续往下走 → **问题 1 关闭**。

---

### 问题 2：`eth_dhcp` 线程栈溢出 → 全系统"假死"

#### 现象
问题 1 修复后，串口能跑到 `DHCP assigned`，但：

- 收 1 帧后**彻底静默**（连 DHCP 重传打印都没有）；
- 串口无任何后续输出，像死机；
- 但并没有 HardFault（`CFSR=0`、`HFSR=0`）。

#### 证据链（这一轮靠 pyOCD 才定位出来）

本机所有 GDB 通道都不可用（CubeProgrammer 写 `DHCSR` 被拒、ST 定制 OpenOCD 报 `hla newtap`、`ST-LINK_gdbserver` 报 Unknown MCU、`arm-none-eabi-gdb` 连不上 3333/61234）。**唯一可用通道是 pyOCD**（`pip install pyocd` → 0.46.1）：

```powershell
python -m pyocd commander -t cortex_m -c "halt" -c "reg pc" -c "reg lr" -c "reg ipsr" -c "reg primask" -c "reg basepri"
```

停机现场实测：

| 寄存器 | 值 | 含义 |
|---|---|---|
| `pc` | `0x0800896e` | `addr2line` → **`Error_Handler (main.c:238)`** |
| `lr` | `0x08008ad3` | → **`vApplicationStackOverflowHook (freertos.c:88)`** |
| `ipsr` | `14` | 在异常上下文里（不是普通任务） |
| `primask` | **1** | **全局关中断**（`Error_Handler` 里的 `__disable_irq()`） |
| `basepri` | `0x50` | 临界区遗留值（此时已无意义） |
| `msp/psp` | `0x2001FFAC` / `0x20003BA0` | — |

再读 `pxCurrentTCB`（符号在 `0x20000158`，值 `0x20003D20`），转储 TCB 在 `0x20003D50` 附近拿到 ASCII `_htepchd` → 反读 = **`eth_dhcp`**。

**结论：`eth_dhcp` 线程栈溢出 → `vApplicationStackOverflowHook` → `Error_Handler` → `__disable_irq()` 死循环**。所以 TIM6/ETH 中断都挂在 `ISPR` 里没人处理、`uwTick` 冻结、串口再无输出。

> 之前所有"只发不收 / RBU 置位 / 收一帧就静默"的现象，本质都是这一次卡死造成的假象。

#### 为什么栈会不够

`dhcp_start` 的调用链极深：

```
dhcp_start → udp_send → ip4_output → etharp_output → netif->linkoutput → low_level_output
```

`low_level_output` 里还有调试 `printf`（picolibc 的 printf 临时开销几百字节）。例程给的 `128*2 = 256 字 = 1 KB` 在这个深度上**必爆**。

#### 修复（栈/堆重排）

| 位置 | 原值 | 新值 | 说明 |
|---|---|---|---|
| `ethernet.c` `LWIP_LINK_STK_SIZE` | `128*2`（1 KB） | **`128*6`（3 KB）** | 链接线程 |
| `ethernet.c` `LWIP_DHCP_STK_SIZE` | `128*2`（1 KB） | **`128*6`（3 KB）** | DHCP 线程 |
| `lwipopts.h` `TCPIP_THREAD_STACKSIZE` | `2048` 字（8 KB，太吃 heap） | **`1280` 字（5 KB）** | 协议栈内核线程（单位是"字"！） |
| `FreeRTOSConfig.h` `configTOTAL_HEAP_SIZE` | `15360` | **`18432`** | 装下所有线程栈 + TCB + 邮箱 |

#### 验证

首次跑通全流程：PHY 正常 → `Start_IT done` → 链路 UP → 发 350 字节 DHCP → 收到 OFFER/ACK → `DHCP assigned: 192.168.2.4` → 连续运行 30 秒无停机，`[CNT]` 心跳一路涨到 `rxirq=125 rdfail=113 errirq=0 errcode=0x0 state=64`（`state=64` = `HAL_ETH_STATE_STARTED`，正常；`errirq=0` 说明从未进错误回调）→ **问题 2 关闭**。

---

### 问题 3（最隐蔽）：Tx 描述符永不回收 → ARP/ICMP 全部发不出去

#### 现象

问题 1、2 都修好后，板子稳定运行、DHCP 拿到 `192.168.2.4`，串口能看到完整的收发：

```
[RX] type=0x0806 len=60 dst=FF:FF:FF:FF:FF:FF src=A8:2B:DD:60:08:16   ← PC 的 ARP 请求，进来了
[TX] len=42 type=0x0806 dst=A8:2B:DD:60:08:16 src=00:2D:00:26:E4:52 arp_op=2 ...  ← 板子回了应答
```

但 PC 侧：

```
Get-NetNeighbor -IPAddress 192.168.2.4
  LinkLayerAddress : 00-00-00-00-00-00
  State            : Unreachable

ping 192.168.2.4
  Request timed out.
  Reply from 192.168.2.45: Destination host unreachable.   ← 本机 ARP 解析失败
```

**即：PC 每帧 ARP 请求都进来了、板子每帧都回了格式完全正确的应答，PC 却始终学不到板子的 MAC、PING 100% 不通。**

#### 走过的弯路（先排除了 3 个"看起来很像"的方向）

| 假说 | 验证方法 | 结果 |
|---|---|---|
| 42 字节 ARP 帧被当 runt（残帧）丢弃 | 在 `low_level_output` 里把 <60 字节的帧补 0 到 60 再发 | **推翻**：补齐后依然不通。STM32F4 MAC 自带 CRCPAD（`TxConfig` 已配 `ETH_TX_PACKETS_FEATURES_CRCPAD`），本就会补 pad + FCS |
| `rdfail ≈ rxirq`（30 秒 113/125）说明每帧丢一半 | 读收包循环逻辑 | **推翻**：循环是"读到没有为止"，末尾那次 `HAL_ETH_ReadData` 失败必然 +1，**1:1 是正常现象** |
| 帧头 src MAC 与 ARP 报文里的 `sha` 不一致 | `[TX]` 打印补 `src=`、`arpSha/arpTha` | **排除**：`dst/arpTha = A8:2B:DD:60:08:16`、`src/arpSha = 00:2D:00:26:E4:52`，完全自洽 |
| 在 PC 侧抓包看帧 | `pktmon filter add` / `start` | **不可用**：需要管理员权限，普通会话返回"拒绝访问" |

#### 决定性证据：UDP 广播探针 + `udp_sendto` 返回值

思路：**广播不需要 ARP**，可以完全绕开 ARP 层，直接回答"板子发的帧到底有没有达到对端"。

在 `ethernetif.c` 临时加探针（每 2 秒一次，`tcpip_callback` 投递到 tcpip 线程里 `udp_sendto` 到 `192.168.2.255:8899`，120 字节负载）：

```
[TX] len=162 type=0x0800 dst=FF:FF:FF:FF:FF:FF src=00:2D:00:26:E4:52   ← lwIP 层"发出"
[UDP] probe seq=15 ret=-12                                            ← 但 udp_sendto 返回 -12 = ERR_IF
```

PC 侧用 PowerShell `UdpClient` 监听 8899：**收到 0 条**。

`ret = -12` 是 lwIP 的 `ERR_IF`，只可能来自 `low_level_output()` 返回 `ERR_IF`，而它只在 **`HAL_ETH_Transmit_IT()` 失败**时返回 → **帧根本没交给 DMA，物理上从未上线**。

#### 最终根因

读 HAL 源码（`Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_eth.c`）：

| 位置 | 事实 |
|---|---|
| `HAL_ETH_IRQHandler()` L1950 | 发送完成（TS 中断）里**只调 `HAL_ETH_TxCpltCallback()`**，**不会**调 `HAL_ETH_ReleaseTxPacket()` |
| `HAL_ETH_ReleaseTxPacket()` L1451 | 描述符回收（`BuffersInUse--`、`releaseIndex` 前移、回调 `txFreeCallback` 释放 pbuf）**全部在这里** |
| 本工程 `HAL_ETH_TxCpltCallback` | 原来是**空函数**，什么都没做 |

于是：

```
上电 → 前 4 帧（正好是 DHCP DISCOVER / REQUEST，3~4 帧）把 ETH_TX_DESC_CNT=4 个描述符占满
     → 描述符永远不回收（BuffersInUse 一直 = 4）
     → 第 5 帧起 HAL_ETH_Transmit_IT() 一直返回 HAL_ERROR
     → low_level_output() 返回 ERR_IF
     → 之后所有 ARP 应答 / ICMP 回包 / 任何单播全部发不出去
```

**这完美解释了"DHCP 能拿到 IP、ARP 却永远不通"这个诡异组合**，也解释了为什么之前所有关于帧长/网卡/交换机的猜测都不成立。

#### 修复

在 `low_level_output()` **开头**回收已发完的描述符：

```c
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  ...
  /* ★每次发包前回收上一批已发完的 Tx 描述符 */
  HAL_ETH_ReleaseTxPacket(&heth);

  ...
  if (HAL_ETH_Transmit_IT(&heth, &TxConfig) != HAL_OK)
  {
#if ETH_DBG_FRAMES
    printf("[TX-FAIL] HAL_ETH_Transmit_IT 返回错误 txlen=%u\r\n", (unsigned)tx_len);
#endif
    pbuf_free(p);
    return ERR_IF;
  }
```

**为什么不放在 `HAL_ETH_TxCpltCallback` 里**：`ReleaseTxPacket` 会回调 `HAL_ETH_TxFreeCallback` → `pbuf_free()`，这会操作 lwIP 内存池（`sys_arch_protect` 临界区）。**在中断上下文里做内存池操作不安全**；`low_level_output` 跑在 tcpip 线程里，每次发包前顺手回收（DMA 微秒级就发完了），既安全又不会卡住发送。同时把 `HAL_ETH_TxCpltCallback` 的注释改成"描述符回收由 low_level_output 负责"，**避免后人又往中断里塞 `pbuf_free`**。

#### 验证

| 项 | 结果 |
|---|---|
| `ping -n 4` | 4/4，0% 丢包，`TTL=255` |
| `ping -n 20` | 20/20，0% 丢包 |
| `ping -n 60` | **60/60，0% 丢包**，min 1 ms / max 3 ms / avg 1 ms |
| `Get-NetNeighbor 192.168.2.4` | `00-2D-00-26-E4-52` / `Reachable` |
| UDP 广播探针 | 连续收到 `M2GW UDP PROBE seq=12/13/14…`（序号连涨） |

→ **问题 3 关闭，Day2 目标达成。**

---

## 八、被推翻的假说清单（别再重复踩）

> 这一章是"负结果"，但价值不亚于正结果。**以下每条都做过实验，证伪了。**

| # | 假说 | 实验 | 结论 |
|---|---|---|---|
| 1 | 改 `SysTick_Handler` 里喂 `HAL_IncTick` 能解决时基问题 | 改 `USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION=1` + 自写 Handler | ❌ 错。HAL 时基是 TIM6，双份递增会让所有 HAL 超时失真。**已回退** |
| 2 | 优先级 0xF0（TIM6）不受 BASEPRI 0x50 屏蔽 | 读 `IPR13` + 实测中断挂起不被处理 | ❌ 错。ARM 语义是屏蔽"数值 ≥ BASEPRI"，0xF0 照样被屏蔽 |
| 3 | 只看 `ISPR` 有挂起 + `BASEPRI=0x50`，就是"卡在临界区出不来" | FreeRTOS `port.c` 插桩实测 `uxCriticalNesting` | ❌ 错。`nest=0`，真因是 `Error_Handler` 里的 `__disable_irq()`（`PRIMASK=1`） |
| 4 | 把 `m2_ethernet_init()` 只搬到任务上下文就够了 | 只搬、不删 `netif_add` 外层临界区 | ❌ 不够。临界区里夹 `HAL_Delay` 是**独立**的缺陷，必须一起删 |
| 5 | ARP 帧 42 字节被交换机/网卡当 runt 丢弃 | 软件补齐到 60 字节重测 | ❌ 推翻。补齐后依然不通；MAC 自带 CRCPAD 本就会补 pad |
| 6 | `rdfail ≈ rxirq` 说明收包丢一半 | 读收包循环 | ❌ 推翻。这是"读到没有为止"的正常终止计数 |
| 7 | 帧头 src MAC 与 ARP `sha` 不一致 | 打印 `src`/`arpSha`/`arpTha` | ❌ 排除。完全自洽 |
| 8 | 用 CubeProgrammer `-w32 0xE000EDF0 0xA05F0003` 写 DHCSR 停核 | 实测 | ❌ 被拒（"Failed to download data!"）。它**只能读不能停核** |
| 9 | 用 ST 定制 OpenOCD / `ST-LINK_gdbserver` / `arm-none-eabi-gdb` 调试 | 逐个试 | ❌ 分别报 `hla newtap` 错 / `Unknown MCU found on target` / `could not connect (error 138)`。**唯一可用通道是 pyOCD** |
| 10 | `pyocd -t stm32f407vg` | 实测 | ❌ 目标名不被识别（列表只有 f412/f429/f439），用通用 `-t cortex_m` |
| 11 | 用 Windows `pktmon` 在 PC 侧抓 ARP | `pktmon filter add -i 192.168.2.4` | ❌ "拒绝访问"（需管理员）。改用 PC 网卡统计计数器 + 板子 UDP 探针替代 |

---

## 九、调试手段与命令速查（可复用）

> 这一章是"工具书"，下次再出问题直接照抄。

### 9.1 串口抓日志（PowerShell，无第三方工具，可后台跑）

```powershell
$sp = New-Object System.IO.Ports.SerialPort('COM6',115200,'None',8,'One')
$sp.ReadTimeout = 1000
$ok=$false; for($i=0;$i -lt 3 -and -not $ok;$i++){ try{ $sp.Open(); $ok=$true } catch { Start-Sleep -Milliseconds 800 } }
$end=(Get-Date).AddSeconds(20); $sb=New-Object System.Text.StringBuilder
while((Get-Date) -lt $end){ try{ $l=$sp.ReadLine(); [void]$sb.AppendLine($l.TrimEnd("`r","`n")) } catch {} }
$sp.Close(); [IO.File]::WriteAllText($logPath, $sb.ToString())
```

要点：**COM6 常被别的后台抓包任务占用**，打开失败按 800 ms 间隔重试 3 次；先确认上一个抓串口任务已结束，否则会抢口。

### 9.2 停核 / 读调用栈（pyOCD，本机唯一可用通道）

```powershell
python -m pyocd commander -t cortex_m `
  -c "halt" -c "reg pc" -c "reg lr" -c "reg ipsr" -c "reg primask" -c "reg basepri" -c "reg msp" -c "reg psp"
```

- 目标名用通用 **`cortex_m`**（`stm32f407vg` 不被识别）。
- 读内存：`read32 <addr> <字节数>`，**长度必须是 4 的倍数**（传 1 会报 `length (1) is not aligned to width (4)`）；命令是 `read32/read16/read8`，不能写 `read`。
- 反查地址属于哪个函数：

```powershell
& "C:\Users\69588\AppData\Local\stm32cube\bundles\gnu-tools-for-stm32\14.3.1+st.2\bin\arm-none-eabi-addr2line.exe" `
  -a -f -e firmware\app\build\modbus-mqtt-gateway.elf 0x0800896e 0x08008ad3
```

- 找全局变量地址：`arm-none-eabi-nm.exe <elf> | Select-String "uwTick|pxCurrentTCB|g_dbg"`

### 9.3 不停机读现场寄存器（HOTPLUG，卡死现场不被复位破坏）

```powershell
& $STM32_Programmer_CLI -c port=SWD sn=066FFF565165485067062111 mode=HOTPLUG -r32 0xE000ED04 4
```

- `-r32` 第二位是**字节数**，合法值 4/8/12/16/32/40；读单个 4 字节字 OK，读 DMA 寄存器一次至少 40 字节。
- 关键地址：`ICSR 0xE000ED04`、`SHCSR 0xE000ED24`、`CFSR 0xE000ED28`、`HFSR 0xE000ED2C`、`MMFAR 0xE000ED34`、`BFAR 0xE000ED38`、`ISPR1 0xE000E204`、`IPR13 0xE000E434`。

### 9.4 PC 侧取证（不允许改网卡配置也能判断）

```powershell
# ① ARP 学习状态：全 0 + Unreachable = 请求发了但应答没被接受
Get-NetNeighbor -IPAddress 192.168.2.4 | Format-List IPAddress,LinkLayerAddress,State

# ② 对照：同网段别的设备能学到，说明 PC 的 ARP 本身正常
Get-NetNeighbor -InterfaceAlias '以太网' -AddressFamily IPv4 | Where-Object State -ne 'Unreachable'

# ③ 收包计数器：对比"安静窗口"与"ping 窗口"的增量，判断帧有没有到
Get-NetAdapterStatistics -Name '以太网' | Format-List ReceivedUnicastPackets,ReceivedBytes,ReceivedDiscardedPackets
```

### 9.5 板子侧主动探针（判定"帧有没有到达对端"的最快办法）

思路：**广播绕过 ARP**。在 `ethernetif.c` 用一个 `tcpip_callback` 投递 `udp_sendto(..., 192.168.2.255:8899)`，PC 侧 `UdpClient` 监听 8899：

```powershell
$udp = New-Object System.Net.Sockets.UdpClient(8899)
$udp.Client.ReceiveTimeout = 2000
$remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
while (...) { try { $b = $udp.Receive([ref]$remote); ... } catch {} }
```

配合**看 `udp_sendto` 的返回值**：返回 `-12 (ERR_IF)` 就直接证明卡在驱动层 `HAL_ETH_Transmit_IT`，比在对端猜帧长快得多。
> 探针代码是纯诊断用，**验证完已删除**（需要时按本章思路重加）。

### 9.6 常用命令速查

| 用途 | 命令 |
|---|---|
| 编译 | `& "C:\Users\69588\AppData\Local\stm32cube\bundles\ninja\1.13.2+st.1\bin\ninja.exe" -C "…\firmware\app\build"` |
| 烧录 | `& "<…>\STM32_Programmer_CLI.exe" -c port=SWD sn=066FFF565165485067062111 mode=UR -w "<elf>" -v -rst` |
| ping | `& 'C:\Windows\System32\PING.EXE' -n 60 -w 1000 192.168.2.4` |
| 改 `.h` 后强制重编 | `(Get-Item <对应.c>).LastWriteTime = Get-Date`（**不要 clean build 目录**） |
| git（不在 PATH） | `& "C:\Program Files\Git\cmd\git.exe" -C <repo> status` |

> 环境坑：本机 PowerShell 里 `ping`、`git`、`pktmon`、`pip` 都不在 PATH，需全路径或 `python -m pip`。

---

## 十、最终验证结果（实测数据）

### 10.1 连通性

```text
Pinging 192.168.2.4 with 32 bytes of data:
Reply from 192.168.2.4: bytes=32 time=1ms TTL=255     ×60

Ping statistics for 192.168.2.4:
    Packets: Sent = 60, Received = 60, Lost = 0 (0% loss),
Approximate round trip times in milli-seconds:
    Minimum = 1ms, Maximum = 3ms, Average = 1ms
```

### 10.2 ARP

```text
PS> Get-NetNeighbor -IPAddress 192.168.2.4
IPAddress   LinkLayerAddress      State
192.168.2.4 00-2D-00-26-E4-52     Reachable
```

### 10.3 运行态寄存器/计数（调试固件实测）

| 项 | 值 | 判读 |
|---|---|---|
| `heth.gState` | `64` | `HAL_ETH_STATE_STARTED`（正常启动） |
| `gdbg_err_irq` | `0` | 从未进 ETH 错误回调 |
| `gdbg_err_code` | `0x0` | 无 DMA 错误 |
| `gdbg_rd_fail` | ≈ `gdbg_rx_irq` | **正常**（读空循环的终止计数，非丢包） |
| `heth.Init.Speed/Mode` | 100M / FullDuplex | 百兆全双工已协商成功 |

> 历史对照（问题 1 卡死阶段的实测，说明当时根本没走到"使能 DMA"这一步）：`DMASR = 0x00000000`、`DMAOMR = 0x02200004`（**ST/SR 位 = 0，DMA 收发未使能**）。

### 10.4 稳定性

- 清理掉全部临时诊断代码后重新编译烧录（`FLASH 109572 B / RAM 113600 B`），再跑 60 次 ping：**依旧 0% 丢包** → 证明**交付代码本身**就能通，不依赖调试打印。
- 长时运行无 hook 触发（串口无 `[HOOK]` / `[ASSERT]`）。

---

## 十一、关键配置参数汇总表

### 11.1 FreeRTOS（`Core/Inc/FreeRTOSConfig.h`）

| 宏 | 值 | 说明 |
|---|---|---|
| `configTOTAL_HEAP_SIZE` | **18432** | 原 15360 装不下多线程栈；改大后 RAM 占用 86.67% |
| `configMAX_PRIORITIES` | 56 | `TCPIP_THREAD_PRIO = 54` |
| `configMINIMAL_STACK_SIZE` | 128 字 | — |
| `configTICK_RATE_HZ` | 1000 | 1 ms tick |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 | 开爆栈检测（配 hook 打印任务名） |
| `configUSE_MALLOC_FAILED_HOOK` | 1 | 开堆耗尽 hook |
| `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` | 5 → `BASEPRI = 0x50` | 中断优先级 ≥5 会被临界区屏蔽 |
| `USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION` | **0** | SysTick 归 cmsis_os2.c；HAL 时基走 TIM6 |

### 11.2 LwIP（`bsp/ethernet/lwipopts.h`）

| 宏 | 值 | 说明 |
|---|---|---|
| `NO_SYS` | 0 | 带 OS |
| `SYS_LIGHTWEIGHT_PROT` | 1 | 用 `sys_arch_protect`（FreeRTOS 临界区） |
| `MEM_SIZE` | `16*1024` | lwIP 自身堆 |
| `MEMP_NUM_PBUF` | 15 | — |
| `PBUF_POOL_SIZE` | 20 | pbuf 池 |
| `MEMP_NUM_TCP_SEG` | 150 | 为后续 MQTT 留 |
| `TCP_MSS` | 1460 | MTU 1500 − 20 − 20 |
| `LWIP_DHCP` | 1 | 开 DHCP |
| `LWIP_ICMP` | 1 | ping 必需 |
| `LWIP_NETIF_LINK_CALLBACK` | 1 | 插拔回调（驱动 DHCP 状态机） |
| `LWIP_NETCONN` / `LWIP_SOCKET` | 1 / 1 | 为后续 MQTT 客户端 |
| `TCPIP_THREAD_STACKSIZE` | **1280**（字，=5 KB） | 原 2048 字（8 KB）太吃 heap |
| `TCPIP_MBOX_SIZE` | 8 | — |
| `TCPIP_THREAD_PRIO` | `configMAX_PRIORITIES-2` = 54 | 内核线程最高档 |

### 11.3 BSP 线程栈/优先级（`bsp/ethernet/ethernet.c`）

| 宏 | 值 | 说明 |
|---|---|---|
| `LWIP_LINK_TASK_PRIO` / `LWIP_LINK_STK_SIZE` | 3 / **128*6 字 = 3 KB** | 原 128*2（1 KB） |
| `LWIP_DHCP_TASK_PRIO` / `LWIP_DHCP_STK_SIZE` | 4 / **128*6 字 = 3 KB** | 原 128*2（1 KB）**导致爆栈停机** |
| `ethernetif_thread` 栈 | 1024 **字节** | CMSIS-RTOS2 该字段以字节计 |
| `LWIP_MAX_DHCP_TRIES` | 10 | 超时回落静态 IP |

### 11.4 网络参数

| 项 | 值 |
|---|---|
| 板子 MAC | `00:2D:00:26:E4:52`（UID 生成） |
| DHCP 实得地址 | `192.168.2.4 / 255.255.255.0 / gw 192.168.2.1` |
| 静态回落 | `192.168.2.30 / 255.255.255.0 / gw 192.168.2.1` |
| `remoteip`（预留） | `192.168.0.120` |
| ETH 中断优先级 | 5（`HAL_NVIC_SetPriority(ETH_IRQn, 5, 0)`） |
| HAL 时基 | TIM6，`TICK_INT_PRIORITY = 15` |
| PHY 地址 / 识别值 | `0x00` / IDR1 `0x0007`、IDR2 `0xC0F1` |

### 11.5 收发关键常量

| 常量 | 值 | 位置 |
|---|---|---|
| `ETH_RX_DESC_CNT` / `ETH_TX_DESC_CNT` | 4 / 4 | `stm32f4xx_hal_eth.h`（HAL 内部） |
| `ETH_RX_BUFFER_CNT` | 10 | `ethernetif.c`（零拷贝 RX 池，≥ 描述符数） |
| `ETH_MIN_FRAME_LEN` | 60 | `ethernetif.c`（短帧补齐，双保险） |
| `ETH_DBG_FRAMES` | **0**（默认关） | `ethernetif.c`，改 1 打开 `[RX]/[TX]/[CNT]` 打印 |

---

## 十二、遗留问题与下一步计划

### 12.1 遗留 / 需关注

| # | 项 | 现状 | 建议 |
|---|---|---|---|
| 1 | RAM 占用 86.67% | 113600 B / 131072 B | 加业务前先用 `uxTaskGetStackHighWaterMark` 查各线程栈余量，必要时把常量/大缓冲挪到 CCMRAM（64 KB 完全空闲） |
| 2 | RX 描述符只有 4 个 | 突发流量下理论仍可能丢帧（当前 ping 不显） | 有实际吞吐需求时再评估加大 `ETH_RX_DESC_CNT`（需改 HAL 头 + `eth.c` 数组大小），或加大 `ETH_RX_BUFFER_CNT` |
| 3 | 短帧补齐属"双保险" | MAC 的 CRCPAD 本就会补 pad+FCS | 可保留（4×60 B RAM + 短帧一次 memcpy），无需删除 |
| 4 | DHCP 租约长期稳定性 | 已验证稳定拿到 `192.168.2.4` 且不掉线，未做长周期（>数小时）测试 | 长时间挂机观察 `[CNT]`/续租打印 |
| 5 | 临时诊断开关 | `ETH_DBG_FRAMES = 0` | 现场排障时改 1，用完改回 0 |
| 6 | 改动未提交 | 工作区有 7 个已跟踪文件修改 + 一批新增文件（untracked） | 提交时**不要**带上 `firmware/app/build/` |

### 12.2 下一步（Day3 起）

1. **TCP 客户端**：连接 MQTT Broker（`LWIP_NETCONN`/`LWIP_SOCKET` 已开，`MEM_SIZE`/`TCP_SEG` 已留余量），打通"连上—心跳—发布/订阅"最小闭环。
2. **Modbus**：`services/modbus` 目录已有骨架，先做 RTU 主机（RS485）或 TCP 从机，注意与 lwIP 的线程模型配合（不要在网络线程里做阻塞 IO）。
3. **状态机与自恢复**：链路断开→重连、DHCP 续租失败→回落静态、MQTT 断线重连。
4. **看门狗**：本次踩的"爆栈→`Error_Handler`→死循环"说明**必须有 IWDG + 独立喂狗线程**，否则现场设备会静默罢工。

---

## 十三、如何复现与注意事项

### 13.1 复现步骤

1. **编译**（不要 clean build 目录）：
   ```powershell
   & "C:\Users\69588\AppData\Local\stm32cube\bundles\ninja\1.13.2+st.1\bin\ninja.exe" -C "C:\Users\69588\Desktop\big project\modbus-mqtt-gateway\firmware\app\build"
   ```
   预期 ELF 体积：`RAM 113600 B (86.67%)` / `FLASH 109572 B (20.90%)`。

2. **烧录**（SWD + 复位）：
   ```powershell
   & "<…>\STM32_Programmer_CLI.exe" -c port=SWD sn=066FFF565165485067062111 mode=UR `
     -w "…\firmware\app\build\modbus-mqtt-gateway.elf" -v -rst
   ```

3. **看串口**（COM6/115200），预期输出见附录 A：`Gateway boot` → PHY 探测 → `[ETH] Start_IT done` → 链路 UP → `DHCP assigned: 192.168.2.4 …`。

4. **PING**：
   ```powershell
   & 'C:\Windows\System32\PING.EXE' -n 20 -w 1000 192.168.2.4
   ```
   预期 0% 丢包；`Get-NetNeighbor -IPAddress 192.168.2.4` 显示 `00-2D-00-26-E4-52 Reachable`。

### 13.2 注意事项（血泪总结）

| # | 注意 | 后果 |
|---|---|---|
| 1 | **`m2_ethernet_init()` 必须在任务上下文调用**（现放 `App_Entry`） | 放回 `osKernelStart` 之前 → `BASEPRI` 锁死 0x50 → 中断全屏蔽 → 卡死 |
| 2 | **临界区里禁止 `HAL_Delay`/`HAL_GetTick`** | TIM6（优先级 15）被屏蔽 → `uwTick` 冻结 → 死循环 |
| 3 | **LwIP 线程栈单位是"字"**（`LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS=1`） | 误按字节配置 → 栈只有应有大小的 1/4 → 爆栈停机 |
| 4 | **不要写 `HAL_ETH_MspInit`**（`eth.c` 已有） | 重复定义编译错 |
| 5 | **不要写 `SysTick_Handler`/`__io_putchar`** | 时基双份递增 / 重定向重复定义 |
| 6 | **`ETH_DBG_FRAMES=1` 时注意串口带宽** | 115200 下帧打印会吃掉大量时间，可能影响时序 |
| 7 | **改 `.h` 后 touch 对应 `.c`** | CMake 依赖没抓到 → 改了等于没改 |
| 8 | **PC 网卡配置不要动**（用户已改回 DHCP） | 改静态会把整条链路搞乱，且不是本次问题的原因 |
| 9 | **`firmware/app/build/` 不要提交、不要删** | 提交会污染仓库；删了要重新 configure |
| 10 | ROM/RAM 余量小，**加功能先算栈/堆** | 重演"爆栈→`Error_Handler`→死机" |

---

## 附录 A：完整启动串口日志示例（干净版本，`ETH_DBG_FRAMES=0`）

```text
Gateway boot
[PHY] IDR1=0x0007 IDR2=0xC0F1 (expect 0x0007/0xC0F1)
[PHY] LAN8720A detected, MDIO OK
[PHY] soft-reset done
[PHY] autonego started
[ETH] sem=0x2000xxxx
[ETH] PRIMASK=0 BASEPRI=0 uwTick=<非 0 且不断增大>
[ETH] Start_IT done
[ETH] init link=UP            （上电时网线已插好；若没插则是 init link=DOWN）
[ETH] rx thread created
The network cable is connected
Looking for DHCP server, please wait...
DHCP assigned: 192.168.2.4 / 255.255.255.0 / gw 192.168.2.1
```

> 说明：本段是按代码里的 `printf` 字符串整理的**典型输出**；`uwTick` 的具体数值每次复位都不同，`init link` 的 UP/DOWN 取决于上电瞬间网线是否插好（后续由 `eth_link` 线程每秒轮询纠正）。

判读要点：
- `PRIMASK=0 BASEPRI=0` → 中断没被锁（问题 1 的体检项）；
- `uwTick` 是个不小的数（如 26）→ TIM6 时基在跑；
- `Start_IT done` 出现 → 没有卡在 `HAL_ETH_Start_IT`；
- `DHCP assigned` → ARP/IP/UDP/DHCP 全链路至少"收"是通的；
- 之后 `ping` 通 → "发"也通（问题 3 已修）。

调试版（`ETH_DBG_FRAMES=1`）额外会打印：

```text
[RX] type=0x0806 len=60 dst=FF:FF:FF:FF:FF:FF src=A8:2B:DD:60:08:16
[TX] len=42 type=0x0806 dst=A8:2B:DD:60:08:16 src=00:2D:00:26:E4:52 arp_op=2 senderIP=192.168.2.4 targetIP=192.168.2.45 arpSha=00:2D:00:26:E4:52 arpTha=A8:2B:DD:60:08:16
[CNT] rxirq=12 errirq=0 rdfail=11 errcode=0x00000000 state=64
```

---

## 附录 B：文件—代码位置索引

| 想查什么 | 去哪 |
|---|---|
| 初始化入口 / 线程创建 | `bsp/ethernet/ethernet.c` `m2_ethernet_init()` |
| DHCP 状态机 | `bsp/ethernet/ethernet.c` `lwip_dhcp_process()` |
| 链路轮询/去抖 | `bsp/ethernet/ethernet.c` `lwip_link_thread()` |
| 网卡初始化（PHY/Start_IT/线程） | `bsp/ethernet/ethernetif.c` `low_level_init()` |
| 发包（含描述符回收修复点） | `bsp/ethernet/ethernetif.c` `low_level_output()` |
| 收包（`HAL_ETH_ReadData`） | `bsp/ethernet/ethernetif.c` `low_level_input()` |
| 收包线程 | `bsp/ethernet/ethernetif.c` `ethernetif_thread()` |
| 零拷贝回调族 | `bsp/ethernet/ethernetif.c` `HAL_ETH_RxAllocateCallback/RxLinkCallback/TxFreeCallback/RxCpltCallback/TxCpltCallback/ErrorCallback` |
| MAC 生成 | `bsp/ethernet/ethernetif.c` `get_stm32_mac()` |
| LAN8720 寄存器操作 | `bsp/ethernet/ethernetif.c` `lan8720_read_reg/write_reg/init/link_read()` |
| LwIP 裁剪配置 | `bsp/ethernet/lwipopts.h` |
| OS 适配（线程/信号量/邮箱） | `bsp/ethernet/sys_arch.c` |
| 内核配置（heap/断言/hook） | `Core/Inc/FreeRTOSConfig.h` |
| hook 与 `vAssertCalled` | `Core/Src/freertos.c` |
| 应用入口（网卡初始化调用点） | `integration/app_entry.c` `App_Entry()` |
| printf 无缓冲 / 内核初始化顺序 | `Core/Src/main.c` USER CODE 2 区 |
| HAL ETH 驱动机制 | `Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_eth.c`：`HAL_ETH_ReadData` L1089、`ETH_UpdateDescriptor` L1202、`HAL_ETH_ReleaseTxPacket` L1451、`HAL_ETH_IRQHandler` L1950 |
| 排障完整过程 | `LWIP_PING排障记录_修正版.md`（第九章 = 最终结论） |
| 快速恢复上下文 | `LWIP_PING_状态快照_重启恢复.md` |

---

### 版本记录

| 日期 | 内容 |
|---|---|
| 2026-11-03 | Day2 移植落地：新增 `bsp/ethernet` 5 个文件 + 搬运 `sys_arch.c` + 引入 LwIP + 三处 CMake/main 接入；编译通过，烧录后卡在 `HAL_ETH_Start_IT` |
| 2026-11-04 上午 | 定位并修复问题 1（初始化位置 + 临界区夹 `HAL_Delay`）、问题 2（`eth_dhcp` 栈溢出 → `Error_Handler`）；系统稳定运行、DHCP 成功 |
| 2026-11-04 下午 | 用 UDP 广播探针定位问题 3（Tx 描述符不回收）→ **PING 打通，60/60、0% 丢包** |
| 2026-11-04 晚 | 清理临时诊断代码（`ETH_DBG_FRAMES=0`、删除探针、清 `port.c` 插桩），重编重烧复验仍 0% 丢包；产出本文档 |

---

## 十四、代码评审修订记录（2026-11-04 下午，commit `bfc2f7a`）

> 本章为合入主线前同事代码评审后的修订。与上文冲突处以本章为准。
> 评审基线：`cf576d2`（已验证快照）；修订后复测：`ping 192.168.2.67` 60/60 0% 丢包，邻居表 `02-2d-00-26-e4-52`。

### 14.1 合入前三项硬修复

| # | 评审意见 | 处理 |
|---|---|---|
| 1 | `netif_set_*/dhcp_start/stop` 在链接/DHCP 线程直接调，与 tcpip 线程竞争 netif 结构 | 全部改走 `netifapi_*`（内部投递到 tcpip_thread 执行）。**隐藏坑**：整个 `src/api/netifapi.c` 被 `#if LWIP_NETIF_API` 包住，lwipopts 里必须显式 `#define LWIP_NETIF_API 1`，否则链接报 undefined reference。 |
| 2 | TX 只在下次发包时回收，最后一批 pbuf 长期占用；软件短帧静态缓冲轮转与描述符释放不绑定，存在 DMA 复用隐患 | ①软件补帧代码**整体删除**——F4 MAC 硬件 CRCPAD 自动补 pad+FCS（`eth.c:73 CRCPadCtrl=ETH_CRC_PAD_INSERT`），实测删后 42 字节 ARP 短帧收发正常；②收包线程每 1s 醒一次时兜底 `HAL_ETH_ReleaseTxPacket()`，空闲时也能归还 pbuf，不新建线程。 |
| 3 | `App_Entry` 网络初始化失败即 `Error_Handler()`，局部网络故障升级为整机停机 | 改为最多重试 5 次（间隔 2s），仍失败则打日志后**继续装配** Modbus/存储，网络保持未就绪待后续重试。 |

### 14.2 文档与规范修正（按评审口径）

- **MAC**：UID 派生 MAC 前 3 字节不属于任何注册 OUI，已加本地管理位（`mac[0]=(mac[0]&0xFC)\|0x02`），实测 `00:2D:..` → `02:2D:00:26:E4:52`；`MACA0HR` bit31 是 F4 手册的 AE 位（地址表项有效），不是通用"源地址有效位"。
- **根因表述更正**（第七/八章以评审口径为准）：①"临界区问题"= `netif_add` 外层 `taskENTER_CRITICAL` 夹住了 `HAL_Delay`，BASEPRI=0x50 屏蔽 TIM6 → uwTick 冻结，**单次读 `HAL_GetTick()` 不会因此卡住**；②"栈溢出问题"是独立事件：`eth_dhcp` 栈爆 → `vApplicationStackOverflowHook` → `Error_Handler` → `PRIMASK=1` 全关中断。二者是两次独立故障，不要合并表述。
- `osKernelInitialize()` 保持 CubeMX 原位置即可，只要在 `MX_FREERTOS_Init()` 之前、网络初始化在调度器启动之后。
- 静态回落 IP 末字节由 UID 派生（原固定 `.30` 多板同网段会冲突）。
- 验证结论口径："**指定固件（commit bfc2f7a）与测试网络环境下，60 次 ping 零丢包**"，不推广为长期结论；长期运行/无 DHCP/拔插恢复/TCP 收发为待补测试。
- 删除"改 .h 必须 touch .c"这条无通用价值的规则；复现步骤以 commit + CMake preset + ELF 路径为准。
- 看门狗后续应按关键任务进展决定喂狗，不搞独立线程无条件喂狗（会掩盖其他任务卡死）。
- 工程当前为 512KiB 直启构建（0x08000000）；Bootloader 的 448KiB 偏移构建另记账，与本文档分开。

### 14.3 仍待办

- 接 MQTT 前实测 FreeRTOS 各任务栈高水位、最低剩余堆、LwIP pbuf 池耗尽次数，据此调 TCP_MSS/pbuf 池。
- PHYAD0/nINTSEL 引脚事实、多板同网段 MAC 冲突测试，请 M4 核对硬件。
- 发送中拔插网线的恢复收发实测（代码路径已通，未做拔线专项测试）。
