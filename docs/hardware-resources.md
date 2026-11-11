# 公共硬件与任务资源

内存、任务及看门狗说明已按2026-11-10的构建和装配源码更新；引脚、外设及DMA表保留2026-11-02硬件基线，后续成员改动以最新`.ioc`和生成代码为准。本文不表示外设已全部实测。资源变更须对齐受影响成员，CubeMX配置及代码生成由用户操作。

## 时钟与内存

| 项目 | 当前配置 |
| --- | --- |
| HSE / 系统主频 | 外部晶体 8MHz；PLL M=4、N=168、P=2，SYSCLK/HCLK=168MHz |
| APB1 / APB2 | 外设时钟 42/84MHz；对应定时器时钟 84/168MHz |
| PLLQ | Q=7，输出 48MHz，供 SDIO 等使用 |
| HAL / RTOS 时基 | HAL 使用 TIM6，FreeRTOS 使用 SysTick，均为 1ms |
| 应用 Flash | Debug/Release从`0x08000000`独立启动；AppDebug从`0x08010000`启动，容量448KiB；Boot容量64KiB |
| 普通 SRAM | 总128KiB；Boot/AppDebug保留首64B邮箱，其余供堆、存储/监督栈及DMA可访问缓冲使用 |
| CCM | `0x10000000`起，64KiB；部分业务栈及网络工作区使用，DMA不可访问 |
| 堆和异常检查 | heap_4 32KiB；静态任务；启用栈溢出和分配失败Hook，实际峰值待验证 |

## 引脚与归属

| 资源 | 引脚 / 别名 | 当前设置及主责 |
| --- | --- | --- |
| RS485 USART3 | PD8 `RS485_TX`、PD9 `RS485_RX`、PD15 `RS485_DIR` | M1；19200、8E1，方向初始低电平接收；UART TC 后才切接收 |
| 维护 USART2 | PD5 `MAINT_TX`、PD6 `MAINT_RX` | M1；115200、8N1，收发 |
| 日志 USART1 | PA9 `LOG_TX`、PA10 `LOG_RX` | M4；115200、8N1，当前仅启用发送，PA10 为预留映射 |
| SPI2 / 外部 Flash | PB10 SCK、PC2 MISO、PC3 MOSI；PE4 `SPI_FLASH_CS` | M3；主机、8bit、模式0、分频8，5.25Mbit/s；片选初始高 |
| SPI 其他片选 | PE7 `SPI_IMU_CS` | 初始高，保持 IMU 未选中；不代表本项目已启用 IMU 功能 |
| I2C1 / RTC | PB6 `I2C1_SCL`、PB7 `I2C1_SDA` | M3；硬件 I2C、100kHz；板载其他 I2C 设备共用总线 |
| TF SDIO | PC8–PC11 D0–D3、PC12 CK、PD2 CMD；PD3 `SD_CARD_DET` | M3；4bit，卡检测输入无内部上下拉；当前不在 main 中自动调用卡初始化 |
| ETH RMII | PA1 REF_CLK、PA2 MDIO、PA7 CRS_DV、PC1 MDC、PC4 RXD0、PC5 RXD1、PB11 TX_EN、PB12 TXD0、PB13 TXD1 | M2；PE12 `ETH_NRST` 初始高；LAN8720A 提供 50MHz REF_CLK |
| TIM7 | 无外部引脚 | M1；1MHz 计数，ARR=2005，初始周期 2006µs；协议驱动按实际帧间隔重设，初始化后未自动启动 |
| SWD | PA13 SWDIO、PA14 SWCLK、NRST、GND | M4 统一保留；板子自行供电时探针必须共地 |

`MX_SDIO_SD_Init` 仍包含生成器的失败即 `Error_Handler` 路径。M3 接入时须提供可报告无卡/故障并恢复的存储初始化，不能直接恢复 main 中的自动调用。SPI2 与 I2C1 当前采用轮询方式，未配置 DMA；这只说明工程配置，不代替驱动实现。

当前 `printf` 只验证了阻塞式启动输出，USART1 TX DMA 配置不代表已经实现多任务日志服务。业务任务暂不直接并发打印，ISR 中不调用 `printf`。

## 看门狗交接约束

统一方案的执行配置已于2026-11-10接入：普通Boot及BootRecoveryTest在GPIO/USART1/SPI2就绪后启动IWDG，收包/认证/安装按固定期限和实际进展刷新；调试/测试启动诊断软件上限15000 ms，普通BootSizeCheck无DEBUG及测试根，但同样具备完整运行路径。最终交接要求已启狗且可信App检查通过，App早期接管后由Monitor监督。仅只读诊断不提前启狗。当前仍为256分频、4095重装值，统一硬件预算及实板复位/时序尚未验收，不能视为已满足拟定≤8 s目标。

超时配置须依据LSI范围、片内Flash擦除取指/中断停顿和最长合法无喂狗区间核算，普通运行卡死复位仍须满足经批准的恢复指标。现有256分频/4095推算约22.31～61.68s，尚未满足需求NFR-REL-04拟定≤8s复位目标；参数及交接时序需实板验证。启动准入与包格式详见[系统设计第8、9章](https://hcn4ticlccda.feishu.cn/docx/JNMDdxoBio9f9bxxQPXcPINSnqh)，不自动改选项字节。

## DMA 与中断

NVIC 使用 4 位抢占优先级、无子优先级。**NVIC 数字越小优先级越高**；调用 FreeRTOS 中断安全 API 的普通外设 IRQ，数值不得小于 5。DMA 的 High/Medium 等仲裁等级与 NVIC 优先级分别配置。

| 请求 | DMA 流 / 通道 | 模式 / DMA 优先级 | DMA IRQ / 外设 IRQ 抢占优先级 |
| --- | --- | --- | --- |
| USART3 RX | DMA1 Stream1 / Ch4 | Circular / High | 5 / USART3=5 |
| USART3 TX | DMA1 Stream3 / Ch4 | Normal / High | 5 / USART3=5 |
| USART2 RX | DMA1 Stream5 / Ch4 | Circular / Medium | 6 / USART2=6 |
| USART2 TX | DMA1 Stream6 / Ch4 | Normal / Medium | 6 / USART2=6 |
| SDIO RX/TX | DMA2 Stream3 / Ch4 | Peripheral Flow Control / Very High；Word、FIFO Full、INCR4 | 5 / SDIO=5 |
| USART1 TX | DMA2 Stream7 / Ch4 | Normal / Low | 7 / USART1=7 |

SDIO生成配置保留一个复用DMA流；当前Storage_Task文件路径使用限时同步轮询回调。SPI2 后续若增加 DMA，须重新检查与 USART3 TX 等流的冲突。ETH 使用独立 DMA，ETH IRQ=5；TIM7 IRQ=5；TIM6、SysTick 和 PendSV 为 15。

## 任务入口与初始预算

**CMSIS-RTOS2 任务优先级数字越大越高**，与上面的 NVIC 规则不同。以下为2026-11-10合并主线后的装配源码配置，不是实测峰值；接入后按完整调用链及栈高水位复核。

| 主责 / 用途 | 初始化 / 任务入口 | 任务优先级 | 静态栈 |
| --- | --- | --- | --- |
| M3 存储 | `Storage_Init` / `Storage_Task` | Normal，24 | 6KiB |
| M1 配置维护 | `Config_Init` / `Config_Task` | BelowNormal，16 | 512B |
| M1 Modbus | `Modbus_Init` / `vModbusPollTask` | AboveNormal，32 | 2KiB |
| M2 MQTT | `MqttClient_Init` / `MqttClient_Task` | Normal，24 | 6KiB |
| M2 遥测 | `Telemetry_Init` / `Telemetry_Task` | BelowNormal，16 | 2560B |
| M4 监督 | `Monitor_Init` / `Monitor_Task` | AboveNormal，32 | 2KiB |
| 默认启动任务 | `StartDefaultTask` → `App_Entry` | Normal，24 | 2KiB |

Storage、Modbus和监督已绑定；Config、MQTT、Telemetry入口仍为`NULL`。六组模块栈的源码配置合计19KiB，实际保留量受链接回收影响，以map为准。存储及监督栈使用普通SRAM，其余模块栈标记为CCM，不能将栈缓冲交给DMA。默认启动任务装配完成后退出；监督任务在网络及业务初始化前启动。ETH接收/tcpip线程由网络适配创建，优先级约定为High（40）/AboveNormal（32），不在装配层重复创建。

初始化在调度器运行后准备本模块基础对象，耗时恢复放进所属任务。就绪状态通过 `app_readiness.h` 上报；初始化或任务创建成功不等于外设/业务就绪。存储队列、暂停/结算/同步和健康报告已接入；真实信任资料、权限及其他模块回调仍待齐备。
