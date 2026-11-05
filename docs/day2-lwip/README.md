# Day2 · LwIP 最小工程移植指南（带 OS 版 · FreeRTOS + LwIP）

> 目标：M2 网关板通电插网线，串口打印 IP，电脑能 ping 通。
> 路线 C：抄正点原子探索者 F407 例程（`Temp\atk_f407\1_Example\SPL\lwip`）。
> **方案演进**：原计划裸跑 NO_SYS → 同事重构工程已把 FreeRTOS 接进 main.c
> （`osKernelStart()` 接管主循环），故 LwIP 改为**带 OS 版**（NO_SYS=0）。
> 例程本身就是 FreeRTOS+LwIP 带 OS 工程，`arch/sys_arch.c`、`arch/lwipopts.h`、
> `arch/netconf.c` 全是现成材料，比裸跑版更省事。
> 当前分支：`feature/day2-lwip`（基于同事重构后的 `origin/main`）。

---

## 一、硬件事实（已从原理图确认，勿再怀疑）

| 项 | 值 |
|---|---|
| 主控 | 天空星核心板 = STM32F407（8MHz HSE → 168MHz，同事 ioc 已配好） |
| PHY | 底板 U17 = **LAN8720A**（RMII 百兆） |
| 时钟 | X1 = 25MHz 晶振 → LAN8720A → CLKOUT 出 **50MHz** → PA1（RMII REF_CLK） |
| RJ45 | J1 = HR911105A（带网络变压器） |
| PHY 复位 | **ETH_NRST（低有效）→ PE12** |

RMII 引脚映射（天空星板）：

| 信号 | 引脚 | 信号 | 引脚 |
|---|---|---|---|
| ETH_REF_CLK | PA1 | ETH_TX_EN | **PB11** |
| ETH_MDIO | PA2 | ETH_TXD0 | **PB12** |
| ETH_CRS_DV | PA7 | ETH_TXD1 | **PB13** |
| ETH_MDC | PC1 | ETH_RXD0 | PC4 |
| ETH_RXD1 | PC5 | PHY 复位 | **PE12** |

> ✅ **必改点已由同事解决**：探索者例程把 TX 组接在 G 口（PG11/PG13/PG14）、
> 复位接 PD3；天空星板是 B 口（PB11/PB12/PB13）+ PE12。同事的 CubeMX 工程
> （`eth.c`/`gpio.c`/`main.h`）已按天空星板重新生成——**引脚相关一个字都不用改**。

---

## 二、同事工程已具备（模板直接复用，勿重复定义）

| 项 | 位置 | 状态 |
|---|---|---|
| USART1 + printf 重定向 | `usart.c`（115200/DMA）+ `main.c` `__io_putchar` | ✅ 现成，**uart 模板作废** |
| ETH 初始化 + RMII 引脚 + 硬件校验和 | `eth.c`（`heth`/`DMARxDscrTab`/`DMATxDscrTab`/`TxConfig` 全局） | ✅ 现成 |
| PHY 复位 PE12 | `gpio.c`（GPIOE12 推挽输出，上电拉高） | ✅ 现成 |
| HAL 驱动源 | `cmake/stm32cubemx/CMakeLists.txt`（hal_eth/hal_uart/FreeRTOS 已接） | ✅ 现成 |
| FreeRTOS | CMSIS_V2 全套 + FreeRTOSConfig.h（Core/Inc） | ✅ 现成 |
| LwIP 协议栈源码 | `Middlewares/LwIP/src`（305 文件）+ `CMakeLists.txt` | ✅ 已就位 |

---

## 三、你要做的（两步搬运 + 四个文件手敲 + 三处 CMake/main 接入）

### 步骤 0：搬运 sys_arch.c（纯搬运，一行不改）

```
源：C:\Users\69588\AppData\Local\Temp\atk_f407\1_Example\SPL\lwip\MiddleWare\lwip-2.2.0\arch\sys_arch.c
目标：firmware/app/bsp/ethernet/sys_arch.c
```

> sys_arch.c 是 lwip 的 FreeRTOS 适配层（sys_sem_new/sys_mbox_new/sys_thread_new/
> sys_now 等），例程原版可直接用，与 HAL/芯片无关。

### 步骤 1：手敲文件清单（每份模板带超详细中文注释）

| 目标文件（bsp/ethernet/ 下） | 模板 | 说明 |
|---|---|---|
| `ethernet.c` / `ethernet.h` | `templates/ethernet.c` / `ethernet.h` | LwIP 接入：tcpip_init + netif_add + DHCP 线程 + 链接线程（逻辑抄例程 netconf.c） |
| `ethernetif.c` / `ethernetif.h` | `templates/ethernetif.c` / `ethernetif.h` | 网卡接口 HAL 零拷贝版：low_level + 收包线程（信号量）+ LAN8720 探测/软复位/链接检测 |
| `lwipopts.h` | `templates/lwipopts.h` | 带 OS 版（NO_SYS=0 + 线程配置 + netconn/socket，照例程原值） |
| `CMakeLists.txt` | `templates/bsp_ethernet_CMakeLists.txt` | m2_ethernet 库（ethernet.c + ethernetif.c + sys_arch.c） |

### 步骤 2：三处接入（详见 `templates/CMakeLists-diff.md`、`templates/main-c-改动.txt`）

1. 顶层 `CMakeLists.txt`：`add_subdirectory(Middlewares/LwIP)` + 链接 `lwip`、`m2_ethernet`
2. `bsp/CMakeLists.txt`：加 `add_subdirectory(ethernet)`
3. `main.c` USER CODE 2 区（printf boot 之后）加一行：`m2_ethernet_init();`

---

## 四、验证路径

1. VS Code Ctrl+B 编译（铁律：VS Code 只能你手动打开）
2. 烧录（DAPLink / ST-Link，天空星核心板自带）
3. 打开串口助手（核心板 USB 口，115200）看打印：
   - `Gateway boot` → `Looking for DHCP server, please wait...` → DHCP 成功打印 IP；
   - 无路由器时 DHCP 超时回落静态：`Static IP: 192.168.1.30 ...`
4. 电脑 ping 板子 IP：
   - 插路由器：ping DHCP 分到的地址
   - 电脑直连：把电脑网卡手动配 `192.168.1.x/24`，ping `192.168.1.30`
5. ping 通 = Day2 达成

---

## 五、方案演进记录（为什么从裸跑转带 OS）

| 时间点 | 方案 | 原因 |
|---|---|---|
| 原计划 | 裸跑 NO_SYS=1，主循环轮询 | 同事工程未接入 FreeRTOS，LwIP 先独立跑 |
| 现在 | 带 OS NO_SYS=0 + 例程 sys_arch/netconf | 同事重构把 FreeRTOS 接进 main.c（osKernelStart 接管主循环），裸跑轮询无处安放；例程本来就是带 OS 工程，材料现成 |

---

## 六、排障速查（编译/链接错误 → 原因）

| 报错 | 原因 |
|---|---|
| `cannot open source file "lwip/opt.h"` | 顶层 CMakeLists 没接入 lwip 库 / 没链接 |
| `arch/cc.h: No such file or directory` | LwIP 源码不自带 `arch/cc.h`（编译器特性头）与 `arch/sys_arch.h`（OS 适配头），需从例程 `lwip-2.2.0/arch/` 拷贝 cc.h/sys_arch.h/cpu.h/perf.h 到 `Middlewares/LwIP/src/include/lwip/arch/`（已拷，勿删） |
| `unknown type name 'osSemaphoreId_t'` | ethernetif.c 没 include `cmsis_os.h`，或 m2_ethernet 没链接 stm32cubemx |
| `undefined reference to 'sys_sem_new'/'sys_now'` | sys_arch.c 没搬运 / 没加进 bsp/ethernet 的 CMakeLists |
| `duplicate definition of 'HAL_ETH_MspInit'` | ethernetif.c 里又写了 MspInit（同事 eth.c 已有，删） |
| `duplicate definition of '__io_putchar'` | main.c 又写了重定向（同事已有，不要写） |
| ping 不通 | 先看串口：没到 DHCP 说明 PHY 探测失败（ID 不对/MDIO 不通）；到了 DHCP 超时说明网线/路由器问题 |
