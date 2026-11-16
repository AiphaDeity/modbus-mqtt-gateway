# 硬件与运行资源

核对日期：2026-11-15。引脚、任务和容量依据当前 App/Boot 源码及 AppDebug、BootSizeCheck 的链接结果；实板结果单独说明。1 KiB = 1024 B，1 MiB = 1024 KiB。

## 当前硬件与引脚

| 资源 | 引脚 / 别名 | 配置与用途 |
| --- | --- | --- |
| RS485 USART3 | PD8 `RS485_TX`、PD9 `RS485_RX`、PD15 `RS485_DIR` | 生成初值 19200、8E1；运行参数由已加载配置设置。方向初始低，TX 完成后切接收 |
| 维护 USART2 | PD5 `MAINT_TX`、PD6 `MAINT_RX` | Boot 恢复口，115200、8N1、无流控、轮询收发；App 不开放串口配置命令 |
| 日志 USART1 | PA9 `LOG_TX`、PA10 `LOG_RX` | 115200、8N1；当前仅发送，PA10 为预留映射 |
| SPI3 / W25Q64 | PB3/SWO SCK、PB4 MISO、PB5 MOSI、PD11 `EXT_SPI_FLASH_CS` | 8 MiB；主机、8 bit、Mode 0、MSB first、软件片选、分频 8；按 APB1=42 MHz 计算 SCK=5.25 MHz |
| 其他 SPI 片选 | PE4 `SPI_FLASH_CS`、PE7 `SPI_IMU_CS` | 保持高电平，原底板 Flash 和 IMU 不被选中 |
| I2C1 / SD3078 RTC | PB6 SCL、PB7 SDA | 100 kHz、轮询；RTC 使用 UTC，后备电源不能代替时间有效性及时间下界检查 |
| TF SDIO | PC8～PC11 D0～D3、PC12 CK、PD2 CMD；PD3 `SD_CARD_DET` | 4 bit；卡检测无内部上下拉，存储任务处理初始化与无卡状态 |
| 用户键 | PA0 | 高电平按下；长按至少 3 s 后松开，选择 TF 配置或升级包，规则见升级说明 |
| ETH RMII | PA1 REF_CLK、PA2 MDIO、PA7 CRS_DV、PC1 MDC、PC4 RXD0、PC5 RXD1、PB11 TX_EN、PB12 TXD0、PB13 TXD1 | PE12 `ETH_NRST`；LAN8720A 提供 50 MHz REF_CLK |
| TIM7 | 无外部引脚 | Modbus 帧间隔定时；1 MHz 计数，生成初值 ARR=2005，驱动按总线参数调整 |
| SWD | PA13 SWDIO、PA14 SWCLK、NRST、GND | 调试接口；与板子共地 |

W25Q64 模块实际接线：3V3→P1-38、GND→P1-39、CS→P1-31/PD11、DO→P2-25/PB4、DI→P2-24/PB5、CLK→核心板调试口 SWO/PB3。SWO 不是 SWCLK。PB4 编码器接口、PB5 LCD 接口保持无外接设备；App 与 Boot 必须同时使用 SPI3。

USART2 转串口使用 3.3 V 电平：适配器 RX→PD5、TX→PD6、GND→板子 GND。恢复工具独占维护口，使用时关闭该口的串口监视器；USART1 日志可由另一端口采集。

时钟配置为 HSE 8 MHz、PLLM=4、PLLN=168、PLLP=2、PLLQ=7，CPU 168 MHz、APB1 42 MHz、APB2 84 MHz、SDIO/RNG 时钟 48 MHz。上述 SPI 频率由配置计算，不是示波器实测上限。

## 片内与外部 Flash 布局

片内 Boot 位于 `[0x08000000,0x08010000)`，末尾 `[0x0800FF80,0x08010000)` 为独立置备的 128 B 信任记录。AppDebug 位于 `[0x08010000,0x08080000)`，可用 448 KiB；正常升级只改 App。独立调试 App 从 `0x08000000` 启动，不能直接作为 Boot 管理的升级镜像。

外部 Flash 地址是相对芯片起点的字节偏移，区间右端不包含；唯一分区定义为 [flash_partition.h](../firmware/shared/flash_partition.h)。

| 区域 | 地址区间 | 容量 |
| --- | --- | --- |
| 配置三槽及保留区 | `[0x000000,0x040000)` | 256 KiB；每槽 80 KiB，JSON 最大 64 KiB |
| 身份、序列及时间等元数据 | `[0x040000,0x080000)` | 256 KiB |
| 控制日志双区 | `[0x080000,0x180000)` | 1 MiB |
| 加密升级包 A 槽 | `[0x180000,0x280000)` | 1 MiB |
| 加密升级包 B 槽 | `[0x280000,0x380000)` | 1 MiB |
| 升级事务日志 | `[0x380000,0x3C0000)` | 256 KiB |
| 保留区 | `[0x3C0000,0x800000)` | 4.25 MiB |

A/B 保存加密包，运行程序只有一份片内 App。回退由 Boot 认证旧包后重新安装；应用运行期间由 Storage_Task 独占 Flash 和 FatFs，Boot 独立使用驱动。

## 当前链接占用

以下是 2026-11-15 目录整理后构建的静态占用及链接保留量，包含已有堆数组、静态栈和缓冲，不能再次叠加计算。普通 SRAM 总 128 KiB，其中首 64 B 为 Boot/App 复位邮箱。

| 构建 | Flash 占用 / 可用 | 普通 SRAM 占用 / 可用 | CCM 占用 / 可用 |
| --- | --- | --- | --- |
| AppDebug | 442.28 / 448 KiB（452896 B） | 118.08 / 127.94 KiB（120912 B） | 58.23 / 64 KiB（59632 B） |
| BootSizeCheck | 53.58 / 63.875 KiB（54868 B） | 18.93 / 127.94 KiB（19384 B） | 0 / 64 KiB |

- App 链接余量：Flash 5.72 KiB、普通 SRAM 9.86 KiB、CCM 5.77 KiB。SRAM 余量与 C 堆可增长空间重合，不能作为另一份独立预算。
- FreeRTOS `heap_4` 数组为 24 KiB，已计入普通 SRAM；C 库堆与它分开，实际分配上限和峰值须结合链接地址及调用链检查。
- mbedTLS 已编入当前工程。TLS 配置为输入内容 6 KiB、输出内容 2 KiB，构建配置对应的记录缓冲为 6189 B / 2093 B；动态分配还包括握手、证书和密码运算，不能只用这两块缓冲代表峰值。
- CCM 不支持 DMA。现有 CCM 栈、队列或密码工作区中的地址不能直接交给 DMA。
- 2026-11-14 冻结 V122 的一次 TF 完整升级测得：C 堆峰值 6040 B / 容量 10608 B、FreeRTOS 最低空闲 5824 B、TF 任务最低栈余 2332 B。这组数据不覆盖 TLS 或真实从站并发。

板上冻结 V122 的 App 为 452952 B；当前构建因源码路径字符串变化减少 56 B，未重新烧录。硬件诊断必须使用与板上镜像匹配的 ELF，不能混用这两组地址或将新构建数字当作新实测。

## 任务与内存归属

下表来自 [app_modules.c](../firmware/app/integration/app_modules.c)。CMSIS-RTOS2 优先级数字越大越高；静态栈不是实际使用峰值。

| 任务 | 入口 | 优先级 | 栈容量 | 所在区域 |
| --- | --- | --- | --- | --- |
| 存储 | `Storage_Task` | Normal / 24 | 6 KiB | 普通 SRAM |
| 配置 | `Config_Task` | Normal / 24 | 1.5 KiB | CCM |
| Modbus | `vModbusPollTask` | AboveNormal / 32 | 2 KiB | CCM |
| MQTT | `MqttClient_Task` | Normal / 24 | 6 KiB | CCM |
| 遥测 | `Telemetry_Task` | Normal / 24 | 2.5 KiB | CCM |
| 监督 | `Monitor_Task` | AboveNormal / 32 | 2 KiB | 普通 SRAM |
| TF 升级 | `UpgradeTf_Task` | Normal / 24 | 6 KiB | 普通 SRAM |
| 网络升级 | `NetworkUpgrade_StartTask` 创建的执行任务 | Normal / 24 | 6 KiB | CCM |
| 默认启动 | `StartDefaultTask` → `App_Entry` | Normal / 24 | 4 KiB | 普通 SRAM |

上述静态栈总量 36 KiB。网络适配另建 ETH 接收、tcpip 线程，FreeRTOS 还有内部任务，不能把此表当作全部 RTOS 占用。默认任务装配后退出；Monitor 在业务初始化前启动。关闭 `GATEWAY_ENABLE_M2` 时不启动 MQTT、遥测和网络升级任务。

Modbus 容量固定为 4 个从站、每站 16 点、32 个寄存器缓存；补传每批最多 4 条。配置、协议字段及写控制限制见 [Modbus 接口说明](../firmware/app/services/modbus/Modbus_Interface_Guide.md)。

## DMA 与中断

NVIC 数字越小优先级越高；当前为 4 位抢占优先级、无子优先级。调用 FreeRTOS 中断安全 API 的普通外设 IRQ 数值不得小于 5。DMA 仲裁等级与 NVIC 优先级是不同设置。

| App 生成配置 | DMA 流 / 通道 | 模式 / 仲裁等级 | DMA IRQ / 外设 IRQ |
| --- | --- | --- | --- |
| USART3 RX | DMA1 Stream1 / Ch4 | Circular / High | 5 / 5 |
| USART3 TX | DMA1 Stream3 / Ch4 | Normal / High | 5 / 5 |
| USART2 RX | DMA1 Stream5 / Ch4 | Circular / Medium | 6 / 6 |
| USART2 TX | DMA1 Stream6 / Ch4 | Normal / Medium | 6 / 6 |
| SDIO RX/TX | DMA2 Stream3 / Ch4 | Peripheral Flow Control / Very High；Word、FIFO Full、INCR4 | 5 / 5 |
| USART1 TX | DMA2 Stream7 / Ch4 | Normal / Low | 7 / 7 |

表中是 App 的生成配置，不代表对应业务已使用 DMA；Boot 维护口使用轮询。当前存储文件路径使用限时同步轮询回调；SPI3、I2C1 未配置 DMA。ETH 使用独立 DMA，IRQ=5；TIM7 IRQ=5；TIM6、SysTick、PendSV=15。

## 看门狗与验证边界

完整 Boot 执行路径提前启动 IWDG，App 早期接管后由 Monitor 根据实际进展、有限等待和健康状态喂狗。当前为 256 分频、4095 重装值；试运行总健康期限 120 s，连续健康 1 s 后请求确认。

2026-11-15 两次试运行故障注入后，复位原因均为 IWDG；主机观察约 33.86 / 33.91 s，随后按两次试运行规则回退。当前阶段已明确优先跑通，不将原 8 s 目标作为本轮阻断条件；这些测量不代表全部温度、电压或 LSI 偏差下的上下限。

SPI3 基线已验证合法恢复、TF 升级、健康确认、坏签名拒绝、试运行失败回退，以及安装、首次试运行、TF 收包、串口恢复收包四个阶段各一个真实断电点。详细验证范围见 [升级说明](../firmware/app/services/upgrade/README.md#已有验证及限制)。真实 MQTT 通知授权、HTTPS/BOS 下载与握手峰值、真实从站并发和长期稳定性仍待验证。
