# 公共硬件与任务资源

依据 `firmware/app/modbus-mqtt-gateway.ioc`、生成代码和 `integration/app_modules.c` 核对，日期为 2026-11-02。本文列当前分配；不表示外设功能已全部实测。修改分配时先对齐受影响成员，由 M4 更新 `.ioc` 并统一生成。

## 时钟与内存

| 项目 | 当前配置 |
| --- | --- |
| HSE / 系统主频 | 外部晶体 8MHz；PLL M=4、N=168、P=2，SYSCLK/HCLK=168MHz |
| APB1 / APB2 | 外设时钟 42/84MHz；对应定时器时钟 84/168MHz |
| PLLQ | Q=7，输出 48MHz，供 SDIO 等使用 |
| HAL / RTOS 时基 | HAL 使用 TIM6，FreeRTOS 使用 SysTick，均为 1ms |
| 应用 Flash | `0x08000000` 起，默认 512KiB 链接区域；当前没有 Bootloader 偏移构建 |
| 普通 SRAM | `0x20000000` 起，128KiB；任务栈、堆和 DMA 缓冲使用此区 |
| CCM | `0x10000000` 起，64KiB；当前保留，禁止交给 DMA |
| 堆和异常检查 | heap_4 15KiB；静态任务；启用栈溢出和分配失败 Hook |

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

SDIO 读写复用一个 DMA 流，由 M3 串行使用并切换方向。SPI2 后续若增加 DMA，须重新检查与 USART3 TX 等流的冲突。ETH 使用独立 DMA，ETH IRQ=5；TIM7 IRQ=5；TIM6、SysTick 和 PendSV 为 15。

## 任务入口与初始预算

**CMSIS-RTOS2 任务优先级数字越大越高**，与上面的 NVIC 规则不同。表中栈大小为字节，是初始预算，接入后按栈高水位修正。

| 主责 / 用途 | 初始化 / 任务入口 | 任务优先级 | 静态栈 |
| --- | --- | --- | --- |
| M3 存储 | `Storage_Init` / `Storage_Task` | Normal，24 | 4KiB |
| M1 配置维护 | `Config_Init` / `Config_Task` | BelowNormal，16 | 4KiB |
| M1 Modbus | `Modbus_Init` / `vModbusPollTask` | AboveNormal，32 | 2KiB |
| M2 MQTT | `Mqtt_Init` / `Mqtt_Task` | Normal，24 | 4KiB |
| M4 监督 | `Monitor_Init` / `Monitor_Task` | BelowNormal，16 | 2KiB |
| 默认启动任务 | `StartDefaultTask` → `App_Entry` | Normal，24 | 2KiB |

五个模块目前均未绑定，入口保持 `NULL`；16KiB 业务栈已静态预留。默认启动任务装配完成后退出。M2 接入时另行创建协议栈要求的 ETH 接收/tcpip 线程，优先级约定为 High（40）/AboveNormal（32），不在装配层重复创建。

初始化在调度器运行后准备本模块基础对象，耗时恢复放进所属任务。就绪状态通过 `app_readiness.h` 上报；初始化或任务创建成功不等于外设/业务就绪。业务队列、持久化、升级和监督实现由对应模块后续接入。
