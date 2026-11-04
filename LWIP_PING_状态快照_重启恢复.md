# LWIP PING 排障 —— 状态快照（重启恢复用）

> 生成时间：2026-11-04（最后更新：2026-11-04 晚，**PING 已通**）
> 用途：电脑重启后快速恢复上下文。结合本目录两份排障文档使用。

---

## 一、当前一句话状态

**已打通。** M2 网关（STM32F407VET6+LAN8720A+FreeRTOS+LwIP）DHCP 拿到 **192.168.2.4**，
`ping -n 60 192.168.2.4` → **60 发 60 收，0% 丢包，平均 1ms**；
PC 邻居表 `192.168.2.4 → 00-2D-00-26-E4-52 Reachable`。

## 二、根因结论（最终版，详见《LWIP_PING排障记录_修正版.md》第九章）

一条链上踩了三个坑，全部已修复并实测验证：

1. **初始化卡死（第一个坑）**：`m2_ethernet_init()` 在调度器启动前执行 + `netif_add` 外层用
   `taskENTER_CRITICAL` 夹住 `HAL_Delay` → 临界区里等 tick 永远等不到。
   → 已删临界区包裹、初始化移入任务上下文。
2. **假死（第二个坑）**：`eth_dhcp` / 链接线程栈只有 `128*2`（1KB），
   `dhcp_start → udp_send → ip4_output → etharp_output → low_level_output` 调用链太深 →
   栈溢出 → `vApplicationStackOverflowHook` → `Error_Handler` → `__disable_irq()` 死循环
   （表现：串口静默、uwTick 冻结、TIM6/ETH 中断挂在 ISPR 里没人处理）。
   → 栈改 `128*6`（3KB）、tcpip 栈 2048→1280 字、heap 15360→18432。
3. **ARP/ICMP 永远发不出去（第三个坑，也是最隐蔽的）**：
   `HAL_ETH_TxCpltCallback` 是空的，从未调 `HAL_ETH_ReleaseTxPacket()` →
   `ETH_TX_DESC_CNT=4` 个 Tx 描述符被前 4 帧（DHCP）占满后永不回收 →
   第 5 帧起 `HAL_ETH_Transmit_IT()` 全部失败 → 帧从未上线。
   → 在 `low_level_output()` 开头调 `HAL_ETH_ReleaseTxPacket(&heth)`。

**指纹**："DHCP 能拿到 IP，但 ARP 应答/ICMP 全发不出去" = 发送资源（描述符/pbuf）没回收。

## 三、工程与命令

- 工程根：`C:\Users\69588\Desktop\big project\modbus-mqtt-gateway`（git 分支 feature/day2-lwip）
- 编译：`& "C:\Users\69588\AppData\Local\stm32cube\bundles\ninja\1.13.2+st.1\bin\ninja.exe" -C "C:\Users\69588\Desktop\big project\modbus-mqtt-gateway\firmware\app\build"`
  - 改 .h 后需 touch 对应 .c 强制重编（`(Get-Item <path>).LastWriteTime = Get-Date`）
- 烧录：`& "D:\stm32cube\STM32CubeIDE_1.17.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.0.202409170845\tools\bin\STM32_Programmer_CLI.exe" -c port=SWD sn=066FFF565165485067062111 mode=UR -w "C:\Users\69588\Desktop\big project\modbus-mqtt-gateway\firmware\app\build\modbus-mqtt-gateway.elf" -v -rst`
- 读寄存器（HOTPLUG 模式，卡死现场不复位）：`-c port=SWD sn=... mode=HOTPLUG -r32 <addr> <count>`
- **停核/读调用栈（唯一可用通道，已装 pyOCD 0.46.1）**：
  `python -m pyocd commander -t cortex_m -c "halt" -c "reg pc" -c "reg lr" -c "reg ipsr" -c "reg basepri"`
  （目标名用通用 `cortex_m`，`stm32f407vg` 不被识别；`read32 <addr> <4N>`，长度必须是 4 的倍数）
- 反查符号：`C:\Users\69588\AppData\Local\stm32cube\bundles\gnu-tools-for-stm32\14.3.1+st.2\bin\arm-none-eabi-addr2line.exe -a -f -e <elf> <addr>`
- 串口：**COM6** / 115200（CH340，U1T=PA9）

## 四、代码改动状态（已落盘、已编译、已烧录验证）

| 文件 | 状态 |
|---|---|
| `bsp/ethernet/ethernetif.c` | **`low_level_output` 开头调 `HAL_ETH_ReleaseTxPacket`（PING 通的最后一环）**；短帧补齐 60 字节（双保险）；`ETH_DBG_FRAMES=0` 已关闭帧打印（可随时改 1）；UDP 广播探针已删除 |
| `bsp/ethernet/ethernet.c` | `LWIP_LINK_STK_SIZE` / `LWIP_DHCP_STK_SIZE` = `128*6`（3KB）；`m2_ethernet_init` 已在任务上下文 |
| `bsp/ethernet/lwipopts.h` | `TCPIP_THREAD_STACKSIZE 1280`（字，=5KB） |
| `Core/Inc/FreeRTOSConfig.h` | `configTOTAL_HEAP_SIZE 18432`；`configASSERT → vAssertCalled` |
| `Core/Src/freertos.c` | `[HOOK] STACK OVERFLOW` / `[HOOK] MALLOC FAILED` / `[ASSERT]` 打印（保留，有用） |
| `portable/GCC/ARM_CM4F/port.c` | `g_dbg_crit_*` 临界区插桩**已清理**（FreeRTOS 中间件恢复干净） |

## 五、待办清单（PING 通过之后）

1. （可选）收包余量：`ETH_RX_DESC_CNT=4` + 零拷贝池 10，繁忙网段下突发时仍可能丢帧，后续可考虑加大。
2. 进入协议层开发：Modbus → MQTT 上云（TCP 连接、心跳、报文解析）。
3. 提交 git（当前改动集中在 `bsp/ethernet/ethernetif.c` 与 `ethernet.c`、`lwipopts.h`、`FreeRTOSConfig.h`、`freertos.c`、`port.c`）。

## 六、文档索引

- `LWIP_PING排障记录.md` —— 第一版（SysTick 方向，已过时，仅参考）
- `LWIP_PING排障记录_修正版.md` —— **权威版本**；其中**第九章是最终结论**（含 Tx 描述符回收根因、UDP 广播探针法、被推翻的假说清单）
- 外部参考：`C:\Users\69588\Documents\xwechat_files\wxid_e4w5u8aedl522_da8f\msg\file\2026-11\M2-LwIP-PING排障分析与处理建议.md`（团队建议；哨兵值机制正确，"TIM6 不被 0x50 屏蔽"表述有误）

## 七、用户约束（勿忘）

- 不再动电脑网卡配置（用户改回 DHCP）
- 不删除 build 目录（用 touch 触发重编）
- 每次排障结论可追加进文档

