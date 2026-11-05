# M2 工业网关 LwIP PING 排障记录（修正版）

> 工程：`modbus-mqtt-gateway`（分支 feature/day2-lwip）
> 芯片：STM32F407VET6 + LAN8720A（RMII）+ FreeRTOS(CMSIS-RTOS2) + LwIP
> 目标：以太网 PING 通板子
> 日期：2026-11-03（第二版，含外部排障建议复核与实测修正）

---

## 一、问题现象

| 项目 | 状态 |
|---|---|
| 串口（COM10/115200） | 已打通，收到 `Gateway boot` |
| PHY 探测 | 完全正常：IDR1=0x0007、IDR2=0xC0F1，LAN8720A 识别、软复位、自动协商均成功 |
| 网线 | 已插路由器，电脑 DHCP 同网段 |
| **卡点** | 串口打印停在 `[ETH] sem=0x20002fe0`，**`[ETH] Start_IT done` 永不打印** → 程序死在 `HAL_ETH_Start_IT(&heth)` 内 |
| 结果 | PING 不通，网卡初始化未走完 |

关键串口日志：

```
Gateway boot
[PHY] IDR1=0x0007 IDR2=0xC0F1 (expect 0x0007/0xC0F1)
[PHY] LAN8720A detected, MDIO OK
[PHY] soft-reset done
[PHY] autonego started
[ETH] sem=0x20002fe0          ← 信号量创建成功
[ETH] PRIMASK=? BASEPRI=? uwTick=?  ← 诊断版新增（待实测）
                               ← 到这里卡死，Start_IT done 未出现
```

---

## 二、完整证据链

### 第 1 步：串口打通（排除打印问题）
- Picolibc `printf` 默认全缓冲 → 打印被吞。修复：`setvbuf(stdout, NULL, _IONBF, 0)`。
- 丝印 U1F 实为 U1T（=USART1 TX=PA9），CH340 RX→U1T、GND→GND，COM10/115200。
- 结果：PHY 探测打印全部出现。

### 第 2 步：排除 RTOS 对象创建顺序问题
- 原 `m2_ethernet_init()` 在 `osKernelInitialize()` **之前**被调用 → 违反 CMSIS-RTOS2，线程/信号量静默失败。
- 修复：`osKernelInitialize()` 前移。
- 结果：信号量创建成功（`sem=0x20002fe0`）。

### 第 3 步：断点打印精确定位卡点
- 在 `low_level_init()` 每步加打印：`sem` → `Start_IT done` → `init link` → `rx thread created`。
- **`Start_IT done` 未出现 → 卡在 `HAL_ETH_Start_IT(&heth)` 内部。**

### 第 4 步：ST-Link HOTPLUG 读寄存器（硬件实测，排除 HardFault）

| 寄存器 | 地址 | 实测值 | 含义 |
|---|---|---|---|
| CFSR | 0xE000ED28 | 0x00000000 | 无内存/总线/用法故障 |
| HFSR | 0xE000ED2C | 0x00000000 | 无 HardFault |
| DMABMR | 0x40029000 | 0x02C12080 | HAL_ETH_Init 已执行 |
| DMARDLAR | 0x4002900C | 0x200052C8 | RX 描述符链已配置 |
| DMATDLAR | 0x40029010 | 0x20005368 | TX 描述符链已配置 |
| DMASR | 0x40029014 | 0x00000000 | 无 DMA 错误 |
| DMAOMR | 0x40029018 | 0x02200004 | TSF\|RSF\|OSF，**ST/SR=0 → DMA 从未被使能** |

**推断**：`HAL_ETH_Init` 完整成功；`HAL_ETH_Start_IT` 卡在**使能 DMA 之前**；CFSR/HFSR=0 → **不是异常，是死循环**。

### 第 5 步：读 HAL 源码，锁定死循环点

`HAL_ETH_Start_IT`（stm32f4xx_hal_eth.c L771-831）关键路径：

```c
ETH_UpdateDescriptor(heth);                    // 构建 RX 描述符（第一轮不卡）
tmpreg1 = MACCR;
HAL_Delay(ETH_REG_WRITE_DELAY);                // ★嫌疑：HAL_Delay(=1)
MACCR = tmpreg1;
SET_BIT(DMAOMR, ST);                           // 使能 DMA 发送（实测没执行到）
SET_BIT(DMAOMR, SR);                           // 使能 DMA 接收（实测没执行到）
```

- `ETH_REG_WRITE_DELAY = 0x00000001U`（L235，**不是 35**）。
- `HAL_Delay`（stm32f4xx_hal.c L390-404）：

```c
uint32_t tickstart = HAL_GetTick();
uint32_t wait = Delay;
wait += uwTickFreq;                            // HAL_Delay(1) → wait = 1 + uwTickFreq
while ((HAL_GetTick() - tickstart) < wait) {} // 等 HAL_GetTick 递增
```

- `HAL_GetTick` 返回 `uwTick`；`HAL_IncTick` 唯一写者：

```c
uwTick += uwTickFreq;                          // uwTickFreq = HAL_TICK_FREQ_1KHZ = 1
```

### 第 6 步：确认 HAL 时基来源 = TIM6（不是 SysTick）

- 工程含 `stm32f4xx_hal_timebase_tim.c`（非弱 `HAL_InitTick`）：配置 TIM6 每 1ms 更新中断，`HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn)`，优先级 `TICK_INT_PRIORITY=15`。
- 调用链：`TIM6 中断 → TIM6_DAC_IRQHandler → HAL_TIM_IRQHandler(&htim6) → HAL_TIM_PeriodElapsedCallback（TIM6）→ HAL_IncTick → uwTick++`。
- **SysTick CTRL(0xE000E010)=0** → SysTick 从未被配置/使用（HAL 不用它）。
- **结论：HAL 时基与 SysTick 无关。第一版文档"SysTick_Handler 不调 HAL_IncTick 导致死循环"的判断方向错误，必须修正。**

### 第 7 步：修复版卡死现场实测（关键数据）

| 项 | 实测 | 含义 |
|---|---|---|
| uwTick @0x20000114 | 0x0000000A = **10** | 因 `HAL_IncTick` 是 `uwTick+=1`，=10 即 **TIM6 中断只被处理了 10ms** |
| TIM6 CR1 @0x40001000 | 0x01 | CEN=1，TIM6 在跑 |
| TIM6 DIER @0x4000100C | 0x01 | UIE=1，更新中断使能 |
| TIM6 SR @0x40001010 | 0x01 | UIF=1，溢出标志挂起 |
| NVIC ISPR1 @0xE000E204 | 0x00400000 | **TIM6 中断挂起但从未被处理** |
| NVIC ISER1 | 0x28C000E0 | bit22=1，TIM6 中断已使能 |

**推断**：TIM6 一直在产生中断（挂起标志累积），但 **CPU 不响应（被屏蔽）** → uwTick 停在 10 → `HAL_Delay` 死循环 → `Start_IT` 卡死。

### 第 8 步：FreeRTOS 调度器启动前的中断屏蔽机制（核心根因）

ARM_CM4F 端口（port.c）关键事实：

```c
static UBaseType_t uxCriticalNesting = 0xaaaaaaaa;   /* ★初始哨兵值，不是 0 */

void vPortEnterCritical(void)
{
  portDISABLE_INTERRUPTS();   /* BASEPRI = configMAX_SYSCALL_INTERRUPT_PRIORITY */
  uxCriticalNesting++;
}

void vPortExitCritical(void)
{
  uxCriticalNesting--;
  if (uxCriticalNesting == 0)
    portENABLE_INTERRUPTS();  /* 只有归零才恢复 BASEPRI=0 */
}
```

- **调度器启动前**：`uxCriticalNesting = 0xAAAAAAAA` → 任意一次 `taskENTER_CRITICAL/taskEXIT_CRITICAL` 配对后，`uxCriticalNesting` 仍为 0xAAAAAAAA ≠ 0 → **BASEPRI 永久锁死在 0x50，不再恢复**。
- `xPortStartScheduler()` 启动时才把 `uxCriticalNesting` 重置为 0。
- 本项目 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5` → `configMAX_SYSCALL_INTERRUPT_PRIORITY = 0x50` → **屏蔽优先级数值 0~5 的所有中断**。
- **`ETH_IRQn` 优先级 = 5 → 被 0x50 屏蔽** → 即使 `HAL_ETH_Start_IT` 完成，**ETH 接收中断也永远不会触发** → 收包线程饿死 → PING 不通。

**注**：外部建议文档称"TIM6（优先级 15）被 0x50 屏蔽"——**该表述不准确**：CM4 优先级编码 0xF0 > 0x50，理论不受 BASEPRI=0x50 屏蔽。被屏蔽的确定对象是 **ETH 中断（优先级 5）**。而实测 uwTick 停在 10，说明 TIM6 中断实际也未得到处理——**TIM6 实际优先级（IPR13）与 PRIMASK 状态尚有一处缺口未实测**（见第五节）。

---

## 三、修正后的根因（因果链）

```
复位 → HAL_Init → TIM6 时基启动（uwTick 开始 +1/ms）
  → main：printf → osKernelInitialize → m2_ethernet_init
  → 网卡初始化在【调度器启动前】执行
      → osSemaphoreNew / osThreadNew 内部进入 FreeRTOS 临界区
      → uxCriticalNesting 哨兵值机制 → BASEPRI 锁死 0x50（永不恢复）
      → ① ETH 中断(优先级5)被屏蔽 → 收包线程永久饿死 → PING 不可能通
      → ② uwTick 停在 10（TIM6 中断实际未再被处理，缺口待实测）
          → HAL_ETH_Start_IT 内 HAL_Delay(1) 死循环
          → Start_IT done 永不打印（当前串口卡点）
```

**一句话**：**网络初始化被放在调度器启动前执行，触发 FreeRTOS 临界区哨兵机制把中断屏蔽锁死；既卡死了 Start_IT 的 HAL_Delay，又屏蔽了 ETH 收包中断。**

---

## 四、第一版判断的修正表

| 原判断（第一版） | 修正 |
|---|---|
| HAL_IncTick 只能由 SysTick 调用，SysTick 不喂 HAL 导致死循环 | **错**。HAL 时基是 TIM6（timebase_tim.c），SysTick CTRL=0 从未使用。**回退 SysTick 相关修改** |
| ETH_REG_WRITE_DELAY=35ms | 实际 `0x00000001U`（1ms 单位，经 uwTickFreq 换算约 2 个 tick） |
| 卡死一定在 HAL_Delay | 卡在 `HAL_ETH_Start_IT` 调用附近，需 PC/调用栈最终确证；之前还有 `ETH_UpdateDescriptor`（RX 分配回调） |
| PHY ID 正确 = PHY 完全正常 | 只证明 MDIO 识别通道正常；RMII 数据收发、50MHz 参考时钟仍待链接/抓包验证 |
| sem 是 SRAM 地址 = RTOS 状态正常 | 地址不能证明调度器状态与对象有效性 |
| 需要给 SysTick 加 HAL_IncTick（自定义 SysTick_Handler） | **撤销**。若保留会在调度器启动后与 TIM6 双份递增 uwTick，造成超时失真 |
| CMake/Ninja 不跟踪头文件，改完必须 touch | 正常构建会维护依赖；touch 只是临时手段，排查实际包含路径为准 |
| printf 缓冲表示日志被吞 | 可能是未刷新；关键日志后 `fflush` 或关缓冲，调试打印不放 ISR |

---

## 五、修改方案（最终）

### 修改 1：恢复公共时基分工（回退第一版 SysTick 改动）

**目标**：TIM6 服务 HAL、SysTick 服务 FreeRTOS，互不干扰，且不重复递增 uwTick。

| 文件 | 动作 |
|---|---|
| `Core/Inc/FreeRTOSConfig.h` L170 | `USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION` 改回 **0**（用 cmsis_os2.c 默认 SysTick_Handler，只喂 FreeRTOS 心跳） |
| `Core/Src/stm32f4xx_it.c` | **删除**自定义 `SysTick_Handler`（恢复原样），避免与 TIM6 双份喂 uwTick |

### 修改 2（★核心）：把网络启动移到调度器运行后的任务上下文

**原理**：调度器启动后 `uxCriticalNesting` 被重置为 0，临界区配对正常，BASEPRI 恢复，ETH 中断解封。

```c
/* main.c —— 只做 HAL/时钟/基础外设 + 内核启动 */
int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  /* ...其它外设初始化... */

  osKernelInitialize();                 /* ① 内核初始化（只初始化，不启动） */
  MX_FREERTOS_Init();                   /* ② 创建默认启动任务（含网络初始化任务） */
  osKernelStart();                      /* ③ ★启动调度器——之后才允许做网络初始化 */
  for (;;) {}
}
```

```c
/* 默认启动任务（在任务上下文里做网络初始化）*/
static void App_Entry(void *argument)
{
  setvbuf(stdout, NULL, _IONBF, 0);     /* 关 printf 缓冲（Picolibc） */
  printf("Gateway boot\r\n");

  m2_ethernet_init();                   /* ★网络初始化移到这里：调度器已运行 */

  for (;;) { osDelay(1000); }           /* 业务主循环 */
}
```

**为什么必须在任务里**：
1. 调度器运行后 `uxCriticalNesting=0`，`osSemaphoreNew/osThreadNew` 的临界区能正确恢复 BASEPRI；
2. ETH 接收中断（优先级 5）不再被 0x50 屏蔽，收包线程能被唤醒；
3. `HAL_Delay` 依赖的 TIM6 时基不再被任何临界区卡住。

### 修改 3：诊断打印保留（下次实测用）

`ethernetif.c` `low_level_init()` 在 `Start_IT` 前打印：

```c
printf("[ETH] PRIMASK=%u BASEPRI=%u uwTick=%lu\r\n",
       (unsigned)__get_PRIMASK(), (unsigned)__get_BASEPRI(),
       (unsigned long)HAL_GetTick());
```

预期：**PRIMASK=0、BASEPRI=0、uwTick 持续增大** → 说明中断未锁、时基正常。

### 修改 4：保持不动（已确认正确）

- `startup_stm32f407xx.s`：SVC/PendSV 弱定义 → Default_Handler，由 FreeRTOSConfig.h 宏映射后 port.c 的强符号自动覆盖（已用 nm 验证）。
- `TICK_INT_PRIORITY = 15`：TIM6 时基优先级，正常。
- PHY/时钟方案：LAN8720A 25MHz 晶振 + nINT/REFCLKO 输出 50MHz 回灌 PA1，eth.c 不配 MCO（正确）。

---

## 六、待实测缺口（板子回来后一次确认）

| 检查项 | 方法 | 判定 |
|---|---|---|
| TIM6 实际优先级 | ST-Link HOTPLUG 读 IPR13 @0xE000E434 | 0xF0=15（理论值）；若 ≠15 则是 uwTick 停的直接原因 |
| PRIMASK | 诊断版固件串口打印 | 0=正常；1=全局关中断 |
| BASEPRI | 诊断版固件串口打印 | 0=正常；0x50=临界区锁死（根因确认） |
| PC/调用栈 | 调试器暂停读 PC | 确证卡在 `HAL_Delay` 的 while |
| uwTick 间隔变化 | 连续两次读 0x20000114 间隔 100ms | 增加=时基在跑；不变=时基停 |

---

## 七、验证流程（按序）

1. **编译**：`ninja -C firmware/app/build`（改 .h 后 touch 对应 .c 触发重编）。
2. **烧录**：`STM32_Programmer_CLI -c port=SWD sn=... mode=UR -w <elf> -v -rst`。
3. **复位看串口**，预期完整输出：

```
Gateway boot
[PHY] IDR1=0x0007 IDR2=0xC0F1 (expect 0x0007/0xC0F1)
[PHY] LAN8720A detected, MDIO OK
[PHY] soft-reset done
[PHY] autonego started
[ETH] sem=0x2000xxxx
[ETH] PRIMASK=0 BASEPRI=0 uwTick=xxxx   ← 诊断值正常（中断未锁、时基在跑）
[ETH] Start_IT done                      ← 卡点解除
[ETH] init link=UP
[ETH] rx thread created
The network cable is connected
Looking for DHCP server, please wait...
DHCP assigned: 192.168.x.x / 255.255.255.0 / 192.168.x.1
```

4. **电脑 PING**：`ping 板子IP`（串口打印的 DHCP 地址），通即成功。
5. **收尾检查**：剩余 FreeRTOS 堆、线程栈高水位、网络缓冲池余量。

---

## 八、避坑经验

| 坑 | 教训 |
|---|---|
| HAL 时基来源必须先确认 | 看是否链接了 `stm32f4xx_hal_timebase_tim.c`（TIM6）还是用 SysTick；别凭印象改 SysTick |
| 调度器启动前创建 RTOS 对象 | `uxCriticalNesting` 哨兵值会让 BASEPRI 锁死；**网络初始化必须放任务上下文** |
| ETH 中断优先级与 BASEPRI | 优先级 5 会被 `configMAX_SYSCALL_INTERRUPT_PRIORITY=5`（0x50）屏蔽——注意配置关系 |
| uwTick 数值含义 | `HAL_IncTick` 是 `uwTick += uwTickFreq`，`uwTickFreq=1`（1KHz）→ 数值即毫秒 |
| "卡死"先用寄存器定性 | HOTPLUG 读 CFSR/HFSR 区分 HardFault 与死循环；UR 模式会复位清掉故障寄存器 |
| 外部建议要复核 | 团队文档的机制（哨兵值）真实，但"TIM6 被 0x50 屏蔽"表述有误——以源码和实测为准 |

---

## 九、最终收尾：PING 正式打通（2026-11-04）

> 本章是**最终结论**，前面第二~五章的根因分析只覆盖了"初始化阶段卡死"，本章补上"卡死解除后仍然 PING 不通"的那一环。
> 结果：**`ping 192.168.2.4` → 60 发 60 收，0% 丢包，平均 1ms，邻居表 `00-2D-00-26-E4-52 Reachable`**。

### 9.1 上一章的根因需要修正（以实测为准）

| 旧结论 | 实测事实 | 修正后表述 |
|---|---|---|
| BASEPRI 锁死 0x50 → ETH 中断被屏蔽 → 卡死 | 用 FreeRTOS `port.c` 插桩实测 `uxCriticalNesting == 0`；停机现场 `PRIMASK=1`、`pc=Error_Handler`、`lr=vApplicationStackOverflowHook` | 真因是 **`eth_dhcp` 线程栈溢出 → hook → `Error_Handler` → `__disable_irq()` 死循环**；`netif_add` 外层用 `taskENTER_CRITICAL` 夹住 `HAL_Delay` 是另一个独立缺陷（已删） |
| 优先级 0xF0（TIM6）不受 0x50 屏蔽 | ARM 语义：BASEPRI 屏蔽的是"数值 ≥ BASEPRI"的中断，0xF0 > 0x50，**确实会被屏蔽** | 旧表述作废 |

两个致命点已在 2026-11-04 上午修复并验证（详见第十章后的变更清单）：
- 删除 `netif_add` 外层的 `taskENTER_CRITICAL/EXIT`（临界区里不能调 `HAL_Delay`）。
- 栈/堆重排：`LWIP_LINK_STK_SIZE`、`LWIP_DHCP_STK_SIZE` 由 `128*2` 改 `128*6`（3KB）；`TCPIP_THREAD_STACKSIZE` 2048→1280（字，=5KB）；`configTOTAL_HEAP_SIZE` 15360→18432。

### 9.2 卡死解除后的新现象（这一轮真正的主角）

系统稳定运行、DHCP 拿到 192.168.2.4、串口持续有收包打印，但：

```
[RX] type=0x0806 len=60 dst=FF:FF:FF:FF:FF:FF src=A8:2B:DD:60:08:16   ← PC 的 ARP 请求，进来了
[TX] len=42 type=0x0806 dst=A8:2B:DD:60:08:16 src=00:2D:00:26:E4:52 arp_op=2 ...  ← 板子回了应答
```

PC 侧却是：

```
Get-NetNeighbor -IPAddress 192.168.2.4
  LinkLayerAddress : 00-00-00-00-00-00
  State            : Unreachable
ping 192.168.2.4 → 交替 "Request timed out" / "Reply from 192.168.2.45: Destination host unreachable"
```

即：**PC 每帧 ARP 请求都进来了、板子每帧都回了格式完全正确的应答，PC 却始终学不到板子的 MAC。**

### 9.3 走过的弯路（都已被实测推翻，别再重复）

| 假说 | 验证方法 | 结论 |
|---|---|---|
| ARP 应答只有 42 字节，被交换机/网卡当 runt 丢弃 | 在 `low_level_output` 里把 <60 字节的帧补 0 到 60 字节再发（已实现并保留，属双保险） | **推翻**：补齐后 PC 依然学不到 MAC。STM32F4 的 MAC 本身带 CRCPAD（`eth.c` 的 `TxConfig` 已配 `ETH_TX_PACKETS_FEATURES_CRCPAD`），会自动补 pad + FCS |
| `rdfail ≈ rxirq`（30 秒 113 / 125）说明每帧丢一半 | 读 `ethernetif_thread` 逻辑 | **推翻**：收包循环是"读到没有为止"，末尾那次 `HAL_ETH_ReadData` 失败必然 +1，1:1 是正常现象，不是丢包 |
| 帧头 src MAC 与 ARP 报文里的 sha 不一致 | 给 `[TX]` 打印补 `src=` 和 `arpSha/arpTha` | **排除**：`dst/arpTha=A8:2B:DD:60:08:16`、`src/arpSha=00:2D:00:26:E4:52`，完全自洽 |
| PC 侧抓包看帧 | `pktmon filter add` / `pktmon start`（需管理权限） | 不可用：`拒绝访问`。改用 **PC 侧网卡统计计数器 + 板子主动发 UDP 广播探针** 替代 |

### 9.4 决定性证据：UDP 广播探针 + `udp_sendto` 返回值

在 `ethernetif.c` 里临时加了个探针：`tcpip_callback` 投递 → `udp_sendto` 到 `192.168.2.255:8899`，每 2 秒一次。广播不需要 ARP，可以完全绕开 ARP 层。结果：

```
[TX] len=162 type=0x0800 dst=FF:FF:FF:FF:FF:FF src=00:2D:00:26:E4:52   ← 帧在 lwIP 层已经"发出"
[UDP] probe seq=15 ret=-12                                            ← 但 udp_sendto 返回 -12 = ERR_IF
```

`ret=-12`（lwIP 的 `ERR_IF`）只可能来自 `low_level_output` 返回 `ERR_IF`，而它只有在 **`HAL_ETH_Transmit_IT()` 失败**时才返回 —— 也就是说 **帧根本没交给 DMA，物理上从未上线**。同时 PC 侧监听 8899 收到 **0 条**，与"没发出去"完全一致。

### 9.5 最终根因：Tx 描述符永不回收

`stm32f4xx_hal_eth.c` 的 `HAL_ETH_IRQHandler()`（L1950）在发送完成（TS）中断里**只调 `HAL_ETH_TxCpltCallback()`，不会调用 `HAL_ETH_ReleaseTxPacket()`**。而描述符的回收（`BuffersInUse--`、`txFreeCallback` 释放 pbuf）全部由 `HAL_ETH_ReleaseTxPacket()` 完成（L1451）。本工程的 `HAL_ETH_TxCpltCallback` 原来是空的：

```c
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth) { (void)heth; }   /* ← 什么都没做 */
```

于是：
- 上电后前 4 次发送（DHCP DISCOVER / REQUEST，正好 3~4 帧）把 `ETH_TX_DESC_CNT=4` 个描述符全部占满并**永不清空**；
- 从第 5 帧起 `HAL_ETH_Transmit_IT()` 一直返回 `HAL_ERROR` → `low_level_output()` 返回 `ERR_IF`；
- 表现就是：**DHCP 能拿到 IP（前几帧还发得出去），之后所有 ARP 应答、ICMP 回包全部发不出去 → PC 永远学不到 MAC → PING 不通。**

这解释了为什么"DHCP 通、ARP 不通"这个诡异组合，也解释了为什么之前所有关于帧长/网卡/交换机的猜测都不成立。

### 9.6 修复（`bsp/ethernet/ethernetif.c`）

在 `low_level_output()` 开头回收已发完的描述符：

```c
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  ...
  /* ★每次发包前回收上一批已发完的 Tx 描述符 */
  HAL_ETH_ReleaseTxPacket(&heth);
  ...
  if (HAL_ETH_Transmit_IT(&heth, &TxConfig) != HAL_OK)
  {
    printf("[TX-FAIL] HAL_ETH_Transmit_IT 返回错误 txlen=%u\r\n", tx_len);  /* 仅 ETH_DBG_FRAMES=1 时 */
    pbuf_free(p);
    return ERR_IF;
  }
```

**为什么放在 `low_level_output` 里而不是 `HAL_ETH_TxCpltCallback` 里**：`ReleaseTxPacket` 会回调 `HAL_ETH_TxFreeCallback` → `pbuf_free()`，这会操作 lwIP 内存池（`sys_arch_protect` 临界区）。放在中断上下文里做内存池操作不安全；`low_level_output` 跑在 tcpip 线程里，每次发包前顺手回收，DMA 早已发完（微秒级），既安全又不会卡住发送。

同时把 `HAL_ETH_TxCpltCallback` 的注释改成"描述符回收由 low_level_output 负责"，避免后人又往中断里塞 `pbuf_free`。

### 9.7 验证结果（2026-11-04 实测）

| 验证项 | 方法 | 结果 |
|---|---|---|
| PING | `ping -n 60 192.168.2.4` | **60 发 60 收，0% 丢包**，min 1ms / max 3ms / avg 1ms |
| ARP 学习 | `Get-NetNeighbor -IPAddress 192.168.2.4` | `LinkLayerAddress = 00-2D-00-26-E4-52`，`State = Reachable` |
| 发送通路 | 板子 UDP 广播探针 + PC 监听 8899 | 连续收到 `M2GW UDP PROBE seq=12/13/14...`（序号连涨） |
| 系统稳定性 | 连续运行 + 长 ping | 无死机、无 hook 触发、`state=64`（`HAL_ETH_STATE_STARTED`）、`errirq=0` |

### 9.8 本轮遗留的调试手段（已清理 / 可复用）

| 手段 | 现状 |
|---|---|
| `[RX]/[TX]/[CNT]` 帧级打印 | 代码保留，`ETH_DBG_FRAMES` 已改 **0** 关闭；以后查链路改 1 即可 |
| UDP 广播探针（8899 口） | **已删除**（纯诊断代码）；需要时可按本章 9.4 的思路重加 |
| `freertos.c` 的 `[HOOK] STACK OVERFLOW` / `[HOOK] MALLOC FAILED` / `[ASSERT]` 打印 | 保留（排查爆栈/断言很有用） |
| `port.c` 的 `g_dbg_crit_nest/g_dbg_crit_idx/g_dbg_crit_lr[]` 临界区插桩 | **已清理**（已证明 `uxCriticalNesting=0`，无继续价值） |
| 短帧补齐到 60 字节 | 保留（规范层面的双保险，非必需；注释已写清真因不是 runt） |

### 9.9 本章新增的排障方法论（值得记住）

1. **"发了但不通"先看 API 返回值**：`udp_sendto`/`esp_...` 返回 `ERR_IF` 就直接定位到驱动层，比在交换机侧猜帧长高效得多。
2. **广播可以绕开 ARP**：判断"板子的帧到底有没有到达对端"时，用 UDP 广播探针 + 对端 `socket` 监听，比抓包更省事（Windows 的 `pktmon` 需要管理员权限，普通会话拿不到）。
3. **PC 侧邻居表是最好的见证者**：`Get-NetNeighbor` 的 `00-00-00-00-00-00 / Unreachable` 表示"ARP 请求发了、应答没被接受"；同网段别的设备是 `Stale/Reachable` 就说明 PC 的 ARP 本身正常，问题在对端。
4. **"DHCP 通但 ARP 不通"是描述符/资源泄漏的典型指纹**：少量前几帧成功、之后全部失败，几乎都是发送资源没回收（TX 描述符、pbuf、DMA 通道）。
5. **改第三方中间件（FreeRTOS `port.c`）的插桩用完就删**，别留在仓库里。

