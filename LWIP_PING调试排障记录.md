# M2 工业网关 LwIP PING 不通 —— 排障记录与修改方案

> 工程：`modbus-mqtt-gateway`（分支 feature/day2-lwip）
> 芯片：STM32F407VET6 + LAN8720A（RMII）+ FreeRTOS(CMSIS-RTOS2) + LwIP
> 目标：以太网 PING 通板子
> 日期：2026-11-03

---

## 一、问题现象

| 项目 | 状态 |
|---|---|
| 串口（COM10/115200） | 已打通，能收到 `Gateway boot` |
| PHY 探测 | 完全正常：IDR1=0x0007、IDR2=0xC0F1，LAN8720A 识别、软复位、自动协商均成功 |
| 网线 | 已插路由器，电脑同网段（DHCP 自动获取） |
| **卡点** | 串口打印停在 `[ETH] sem=0x20002fe0`，**`[ETH] Start_IT done` 从未出现** → 程序死在 `HAL_ETH_Start_IT(&heth)` 调用处 |
| 结果 | PING 不通，网卡初始化没走完 |

关键串口日志（最后一次烧录带断点打印）：

```
Gateway boot
[PHY] IDR1=0x0007 IDR2=0xC0F1 (expect 0x0007/0xC0F1)
[PHY] LAN8720A detected, MDIO OK
[PHY] soft-reset done
[PHY] autonego started
[ETH] sem=0x20002fe0        ← 信号量创建成功（有效 RAM 地址）
                            ← 到这里就没了！Start_IT done 没打印
```

---

## 二、排查过程（完整证据链）

### 第 1 步：排除打印问题（Picolibc stdout 全缓冲）

- **现象**：只有 `Gateway boot` 能显示，PHY/网络打印全被吞。
- **原因**：工程用 Picolibc 库，`printf` 默认全缓冲，内容攒在缓冲区，不满不调用 `_write` 写串口。
- **修复**：`main.c` 里加 `setvbuf(stdout, NULL, _IONBF, 0)` 关掉缓冲。
- **结果**：PHY 探测打印全部出现。

### 第 2 步：排除 RTOS 对象创建失败

- **现象**：PHY 打印后一片空白。
- **原因**：`m2_ethernet_init()` 在 `osKernelInitialize()` **之前**被调用，内部大量 `osThreadNew/osSemaphoreNew` 违反 CMSIS-RTOS2 规范，线程/信号量静默创建失败。
- **修复**：`osKernelInitialize()` 前移到 `m2_ethernet_init()` 之前。
- **结果**：信号量创建成功（`sem=0x20002fe0`）。

### 第 3 步：加断点打印，精确定位卡点

在 `ethernetif.c` 的 `low_level_init()` 每个关键步骤后加打印：

- `[ETH] sem=0x20002fe0`（信号量创建成功）
- `[ETH] Start_IT done`（HAL_ETH_Start_IT 返回）—— **没出现！**
- `[ETH] init link=UP/DOWN`、`[ETH] rx thread created`（更后面的步骤）

**结论：程序死在 `HAL_ETH_Start_IT(&heth)` 内部，没有返回。**

### 第 4 步：ST-Link 读寄存器，用硬件实测排除 HardFault

用 `STM32_Programmer_CLI` 在 **HOTPLUG 模式（不复位）** 读取 CPU 与 ETH 寄存器：

| 寄存器 | 地址 | 实测值 | 含义 |
|---|---|---|---|
| CFSR（可配置故障状态） | 0xE000ED28 | 0x00000000 | **无内存/总线/用法故障** |
| HFSR（硬故障状态） | 0xE000ED2C | 0x00000000 | **无 HardFault** |
| DMABMR（DMA 总线模式） | 0x40029000 | 0x02C12080 | HAL_ETH_Init 已执行 |
| DMARDLAR（RX 描述符链表地址） | 0x4002900C | 0x200052C8 | RX 描述符链已配置 |
| DMATDLAR（TX 描述符链表地址） | 0x40029010 | 0x20005368 | TX 描述符链已配置 |
| DMASR（DMA 状态） | 0x40029014 | 0x00000000 | 无 DMA 错误/中断 |
| DMAOMR（DMA 操作模式） | 0x40029018 | 0x02200004 | TSF\|RSF\|OSF，**ST/SR=0 → DMA 从未被使能** |

**关键推断**：
- `HAL_ETH_Init` **已经完整成功**（总线模式、描述符链表都配置了）。
- `HAL_ETH_Start_IT` **卡在"使能 DMA"之前**（DMAOMR 的 ST/SR 位 = 0）。
- 不是 HardFault（CFSR/HFSR 全 0）→ **是死循环**（代码卡在某个 while 里）。

### 第 5 步：读 HAL 源码，锁定死循环点

`HAL_ETH_Start_IT`（stm32f4xx_hal_eth.c）内部流程：

```c
HAL_StatusTypeDef HAL_ETH_Start_IT(ETH_HandleTypeDef *heth)
{
  if (heth->gState == HAL_ETH_STATE_READY)
  {
    ...
    ETH_UpdateDescriptor(heth);              // ① 构建 RX 描述符
    tmpreg1 = (heth->Instance)->MACCR;
    HAL_Delay(ETH_REG_WRITE_DELAY);          // ② ★嫌疑点：HAL_Delay！
    (heth->Instance)->MACCR = tmpreg1;
    SET_BIT(DMAOMR, ST);                     // ③ 使能 DMA 发送（实测没执行到）
    SET_BIT(DMAOMR, SR);                     // ④ 使能 DMA 接收（实测没执行到）
    ...
  }
}
```

`HAL_Delay` 的实现（stm32f4xx_hal.c）：

```c
__weak void HAL_Delay(uint32_t Delay)
{
  uint32_t tickstart = HAL_GetTick();
  ...
  while ((HAL_GetTick() - tickstart) < wait) {}   // ← 等 HAL_GetTick 递增
}
```

**`HAL_Delay` 依赖 `HAL_GetTick` 递增**。而 `HAL_GetTick` 返回全局 `uwTick`，`uwTick` 只由 `HAL_IncTick()` 递增，`HAL_IncTick()` 只在 **SysTick_Handler** 里被调用。

---

## 三、根因详解

### ★ 根因（致命）：SysTick_Handler 不喂 HAL 时基，HAL_Delay 死循环

**本工程的 SysTick_Handler 由 cmsis_os2.c 提供**（FreeRTOS 的 CMSIS-RTOS2 封装层），其实现：

```c
/* cmsis_os2.c 里的默认实现（USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION == 0 时启用） */
void SysTick_Handler (void)
{
  SysTick->CTRL;                              // 只清溢出标志
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
    xPortSysTickHandler();                    // 只喂 FreeRTOS 心跳
  }
}
```

**它从头到尾没有调用 `HAL_IncTick()`！** 注释也明说了"不要用 HAL_IncTick（weak 函数可能被别的实现替换）"。

于是形成一条**致命的因果链**：

```
HAL_ETH_Start_IT
   └─ HAL_Delay(ETH_REG_WRITE_DELAY)        // 等 35ms
        └─ while (HAL_GetTick() - tickstart < 35) {}
             └─ HAL_GetTick() 恒返回 0       // uwTick 从不递增
                  └─ (0 - 0) < 35 永远成立
                       └─ ★ 死循环！CPU 卡死在这，后面的打印全部消失
```

**为什么 `HAL_GetTick` 恒为 0**：

```
uwTick（全局变量，初始 0）
   └─ 只由 HAL_IncTick() 递增
        └─ 只在 SysTick_Handler 里被调用
             └─ 本工程 SysTick_Handler = cmsis_os2.c 版（不调 HAL_IncTick）
                  └─ uwTick 永远是 0
```

**为什么这么隐蔽**：
1. 串口只打印到 PHY 探测为止，第一反应都以为是 PHY/链接问题；
2. 断点打印加上之前，根本不知道卡在 `HAL_ETH_Start_IT` 里；
3. `HAL_Delay` 死循环不是异常，不会进 HardFault，CFSR/HFSR 全是 0，用调试器看"没有报错"；
4. 需要读到寄存器层（DMAOMR 的 ST/SR=0）+ HAL 源码层才能串起来。

### 附带发现（无需修改，验证正常）

**startup 文件的 SVC/PendSV 弱映射问题**：`startup_stm32f407xx.s` 里 `SVC_Handler`/`PendSV_Handler` 弱定义为 `Default_Handler`，而 `FreeRTOSConfig.h` 里有宏：

```c
#define vPortSVCHandler    SVC_Handler
#define xPortPendSVHandler PendSV_Handler
```

宏替换后，FreeRTOS 的 port.c 实际导出的符号名就是 **`SVC_Handler`/`PendSV_Handler`（强符号）**，链接时自动覆盖 startup 的弱定义。用 `arm-none-eabi-nm` 验证过 `port.c.obj` 里有 `T SVC_Handler`、`T PendSV_Handler`。**该处原本正确，无需修改**（排查中一度误改，已恢复）。

---

## 四、修改方案（最终修改清单）

### 修改 1：`Core/Src/main.c` —— 解决 printf 缓冲 + RTOS 初始化顺序

```c
  /* USER CODE BEGIN 2 */
  setvbuf(stdout, NULL, _IONBF, 0);       // ① 关掉 printf 全缓冲（Picolibc 默认全缓冲会吞打印）
  printf("Gateway boot\r\n");
  osKernelInitialize();                    // ② ★必须先初始化 RTOS 内核！
  m2_ethernet_init();                      //    再创建线程/信号量（内部大量 osThreadNew/osSemaphoreNew）
  /* USER CODE END 2 */
  MX_FREERTOS_Init();
  osKernelStart();
```

### 修改 2：`bsp/ethernet/ethernetif.c` —— 信号量先建再启动 ETH

```c
  RxPktSemaphore = osSemaphoreNew(1, 0, NULL);   // ① 先建收包信号量
  printf("[ETH] sem=%p\r\n", (void *)RxPktSemaphore);

  HAL_ETH_Start_IT(&heth);                       // ② 再启动 ETH（中断回调会用上面的信号量）
  printf("[ETH] Start_IT done\r\n");
```

（顺序反了的话，ETH 中断可能在信号量创建前到达，回调里 `osSemaphoreRelease(NULL)` 崩溃。）

### 修改 3：`Core/Inc/FreeRTOSConfig.h` —— 启用自定义 SysTick_Handler（★核心修复）

```c
/* 原值 0，改为 1：让 cmsis_os2.c 不再提供默认 SysTick_Handler，
   改由 stm32f4xx_it.c 自定义实现（必须同时喂 HAL 时基和 FreeRTOS 心跳） */
#define USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION 1
```

### 修改 4：`Core/Src/stm32f4xx_it.c` —— 自定义 SysTick_Handler（★核心修复）

```c
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "task.h"
/* USER CODE END Includes */

/* USER CODE BEGIN 1 */
void SysTick_Handler(void)
{
  HAL_IncTick();   // ① 喂 HAL 时基：HAL_GetTick 递增，HAL_Delay 才能退出（修复死循环的关键）
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
    xPortSysTickHandler();   // ② 喂 FreeRTOS 心跳：任务调度/延时（调度器启动后才调）
  }
}
/* USER CODE END 1 */
```

### 修改 5：`startup_stm32f407xx.s` —— 无需修改（保持原样）

`SVC_Handler`/`PendSV_Handler` 保持弱定义 → `Default_Handler`，由 port.c 的强符号自动覆盖。

---

## 五、修改后的验证流程

1. **编译**：`ninja -C firmware/app/build`（增量编译通过，RAM 110264B/128KB、FLASH 108952B/512KB）。
2. **烧录**：`STM32_Programmer_CLI -c port=SWD sn=... mode=UR -w <elf> -v -rst`。
3. **复位看串口**，预期完整输出：

```
Gateway boot
[PHY] IDR1=0x0007 IDR2=0xC0F1 (expect 0x0007/0xC0F1)
[PHY] LAN8720A detected, MDIO OK
[PHY] soft-reset done
[PHY] autonego started
[ETH] sem=0x2000xxxx
[ETH] Start_IT done            ← 这行出现 = 卡点解除！
[ETH] init link=UP             ← 网线已插，PHY 链接已建立
[ETH] rx thread created
The network cable is connected ← 链接回调
Looking for DHCP server, please wait...
DHCP assigned: 192.168.x.x / 255.255.255.0 / 192.168.x.1  ← 拿到 IP
```

4. **电脑上 PING**：`ping 板子IP`（串口打印的 DHCP 分配的地址），通即成功。

---

## 六、经验总结（下次避坑）

| 坑 | 教训 |
|---|---|
| Picolibc printf 全缓冲 | 嵌入式移植网络打印，先 `setvbuf(stdout,NULL,_IONBF,0)` |
| osKernelInitialize 顺序 | 所有 `osThreadNew/osSemaphoreNew` 必须在内核初始化之后 |
| HAL_Delay 在 FreeRTOS 工程 | HAL 时基（uwTick）必须有人在喂；SysTick_Handler 要同时喂 HAL 和 FreeRTOS |
| 调试"卡死" | 先区分：HardFault（看 CFSR/HFSR）还是死循环（寄存器停在中间状态）——用 ST-Link HOTPLUG 读寄存器，别用 UR（会复位清空故障寄存器） |
| 头文件依赖 | 手写 CMake 工程 ninja 可能没追踪头文件依赖，改 .h 后要强制重编相关 .c（touch 源文件触发） |
