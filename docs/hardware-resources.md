# 公共硬件与任务资源

整机源码内存预算更新于2026-11-12；任务表按当前装配源码核对。引脚、外设及DMA表保留2026-11-02硬件基线，后续成员改动以最新`.ioc`和生成代码为准。本文不表示外设已全部实测。资源变更须对齐受影响成员，CubeMX配置及代码生成由用户操作。

## 时钟与内存

| 项目 | 当前配置 |
| --- | --- |
| HSE / 系统主频 | 外部晶体 8MHz；PLL M=4、N=168、P=2，SYSCLK/HCLK=168MHz |
| APB1 / APB2 | 外设时钟 42/84MHz；对应定时器时钟 84/168MHz |
| PLLQ | Q=7，输出 48MHz，供 SDIO 等使用 |
| HAL / RTOS 时基 | HAL 使用 TIM6，FreeRTOS 使用 SysTick，均为 1ms |
| 应用 Flash | Debug/Release从`0x08000000`独立启动；AppDebug从`0x08010000`启动，容量448KiB；Boot容量64KiB |
| 升级信任资料 | 2026-11-11软件约定使用独立OTP块12～15：数据`[0x1FFF7980,0x1FFF7A00)`共128 B，锁字节`[0x1FFF7A0C,0x1FFF7A10)`；Boot/App只读，不占主Flash或外部包槽，未执行板上写入/锁定 |
| 普通 SRAM | 总128KiB；Boot/AppDebug保留首64B邮箱，其余供堆、存储/监督栈及DMA可访问缓冲使用 |
| CCM | `0x10000000`起，64KiB；部分业务栈及网络工作区使用，DMA不可访问 |
| 堆和异常检查 | heap_4 24KiB候选已由用户生成并构建；静态任务；栈溢出和分配失败Hook保留，实际峰值待验证 |

## 整机源码内存预算（2026-11-12）

24 KiB候选已由用户在CubeMX生成，278个USER CODE区域均保留；随后接入MQTT和Telemetry自身的进展监督及真实暂停结算，四个应用预设构建通过。当前AppDebug实际C堆跨度为11416 B，扣除7852 B保守对象预算后余3564 B；FreeRTOS正常模型为20088 B，24 KiB预留扣除该模型后余4488 B。这些余额不是实测空闲量，碎片、高水位和最大连续块仍待验证。任务栈、队列和业务容量不变；接收层重复副本已去除，候选尚未完成运行准入。

### 基线与核算方法

- 本地任务分支为 codex/m4-sync-gitee-20261111，基线提交 a866738 包含 Gitee bc38d97；下表为该基线加生命周期修复、用户生成24 KiB候选、M2进展监督、真实暂停结算、CCM零初始化拆分及Stage输入副本优化和真实Boot结果交接后的工作区构建，普通 AppDebug 使用 ST ARM GCC 14.3.1+st.2。没有采用队友未合入源码或重新联网更新分支。
- 读取实际 ELF/map，使用 ARM 调试类型信息核对 sizeof，并按各库实际编译命令预处理选项。临时分析仅运行 Python、编译器预处理及只读调试工具，未执行本机 C 模型、固件或实板。
- FreeRTOS heap_4、LwIP 堆/内存池和 C 库堆相互独立。静态任务、队列和池已在链接占用中，只计一次；Boot/App 分时运行，RAM 不相加。
- 未实现的 Config、HTTPS 及其新增对象不计成已有实现，也不能按 0 B 宣称资源足够；完整需求的准入仍未关闭。

### 当前链接与静态分配

| 场景 | Flash | 普通 SRAM | CCM | 条件 |
| --- | ---: | ---: | ---: | --- |
| AppDebug | 352176/458752 B | 120104/131008 B | 60828/65536 B | 偏移 0x10000，64 B 邮箱另留 |
| Release | 209588/524288 B | 120040/131072 B | 60832/65536 B | 原址启动，不能代替偏移 App |
| AppFlashDebug | 352176/524288 B | 120104/131072 B | 60828/65536 B | 原址调试 |
| M3Test | 231676/524288 B | 61120/131072 B | 27096/65536 B | M2 关闭，不能代替整机预算 |
| BootSizeCheck | 54604/65536 B | 19384/131008 B | 0 B | 2026-11-12提交前重建；Boot日志文字已更新，未实板验证 |

AppDebug 普通 SRAM 的分组如下。所有权和 DMA 范围以真实调用链为准，不能单凭名称决定迁入 CCM。

| 用途 | 已链接容量 | 归属与用途 |
| --- | ---: | --- |
| FreeRTOS ucHeap | 24576 B | heap_4 静态预留，供动态线程和 IPC |
| ETH RX_POOL | 25091 B | 16 块零拷贝接收池，ETH DMA 可访问 |
| LwIP PBUF_POOL | 18387 B | 12 块协议栈 pbuf 池，普通 SRAM |
| LwIP ram_heap | 10259 B | MEM_SIZE=10 KiB，加管理及对齐；不是 12 KiB |
| 普通 SRAM 静态任务栈 | 19968 B | Storage 6144、TF 6144、Monitor 2048、启动 4096、Idle 512、Timer 1024 B |
| 链接器最小 C 堆/MSP 预留 | 1536 B | 512+1024 B，已计入总占用 |
| 其余已链接数据、控制块、缓冲及对齐 | 20287 B | FatFs、驱动、RTOS 状态、升级工作区及其他库数据 |
| 合计 | 120104 B | 链接余量 10904 B，不是完整运行余量 |

CCM 的 24 个有大小的符号合计 60828 B，与段大小一致：

| 用途 | 容量 | 生命周期与边界 |
| --- | ---: | --- |
| 模块静态栈及网络升级 TCB | 17500 B | Config/Modbus/MQTT/Telemetry/网络升级栈；TCB 为 92 B |
| MQTT 工作区 | 23520 B | TX、RX、命令/回执、补传响应及 QoS1 pending；任务间按原同步规则访问 |
| M2/M3 存储桥 | 15832 B | 组装响应及四个静态队列，值拷贝；不再计入 heap_4 |
| Telemetry 工作区 | 896 B | 点位快照及输出缓冲 |
| 网络升级其他工作区 | 3080 B | 通知、topic、HTTP 头及下载块 |
| 合计 | 60828 B | 余 4708 B，DMA 不可访问 |

完整普通 SRAM/CCM 符号清单已在本地核算证据保留；小对象归入上表“其余”不等于未计入总量。

零初值 CCM 对象使用 `.bss.ccmram` 输入节，由链接脚本放入 `.ccmram_bss (NOLOAD)`，不保存 Flash 装载副本；带初值的 `.ccmram` 仍保留装载数据。`AppMemory_InitCcm` 通过 `.preinit_array` 在构造函数和 `main` 前开启 CCM 时钟、复制初值并清零。上述CCM节拆分本身不改变对象的地址和容量；不得将非零初值放入零初始化节。

CCM节拆分阶段，AppDebug Flash 从 401.95 KiB 降到 342.66 KiB，净回收 59.30 KiB，偏移应用区剩余 105.34 KiB；这不是 SRAM 余量。四个应用预设构建、初始化单元严格编译/静态分析、ELF 启动入口及两个链接脚本的非零初值保留/容量边界检查通过；冷启动、软件复位和 Boot 跳转仍待实板验证。

Stage已去除1016 B静态输入副本，同步写入和读回比较期间借用调用方数据；调用方在返回前保持数据可读且不变。TF文件缓冲和网络下载缓冲均在同步调用返回后才复用；存储适配仍将每个≤256 B请求值拷贝给存储任务，DMA不直接访问输入。协议块上限仍为1016 B，读回缓冲仍为256 B。

Stage副本优化阶段，四预设普通SRAM均净减1016 B（约0.99 KiB），当时AppDebug的C堆跨度增至11.17 KiB。修改前后实际函数的29组编译期边界对照及Stage单元严格编译/静态分析通过；未执行原生C、真实RTOS或实板读写，运行高水位仍待测量。

真实Boot结果交接新增普通SRAM：AppDebug/AppFlashDebug为24 B，Release为16 B，M3Test为8 B，含只读后端函数指针、快照判定和PUBACK跟踪状态及对齐；CCM、任务栈、RTOS池和队列容量均不变，没有新增动态分配。AppDebug Flash增加1312 B，当前343.92 KiB，偏移区剩104.08 KiB。C堆跨度为11.15 KiB，扣除原7852 B对象模型后预算余量3.48 KiB，均非实测空闲或最大连续块。

结果核对在现有升级任务中执行，网络任务只读RAM快照；单次状态读取最多3 s，与收包互斥。七个相关C单元严格编译/静态分析、四个应用预设及82组编译期身份/互斥/失败/重连/PUBACK检查通过，另核对三种事件的JSON布局和长度；均未执行原生C、RTOS、broker或实板。新增路径的Debug自身静态栈帧为结果轮询264 B、网络结果发送432 B，不是完整调用链峰值或高水位；栈容量未据此扩大。

### FreeRTOS 正常运行的分配模型

实际 ARM 类型为 TCB_t=92 B、Queue_t=80 B、Timer_t=44 B、BlockLink_t=8 B、modbus_t=24 B、mb_write_request_t=76 B。heap_4 每块按 B(n)=align8(n+8) 核算。

实际选项为 LWIP_TCPIP_CORE_LOCKING=1、LWIP_NETCONN_SEM_PER_THREAD=0、MEMP_MEM_MALLOC=0、MEM_LIBC_MALLOC=0、MEMP_NUM_NETCONN=4、MEMP_NUM_TCP_PCB=2。4 个 netconn 是 IPC 的保守数量上限，不代表可同时建立 4 个 TCP 连接。

| 对象 | 数量及输入容量 | 含块头、对齐的占用 |
| --- | --- | ---: |
| tcpip 线程 | 栈 5120 B + TCB | 5232 B |
| ETH 接收线程 | 栈 1024 B + TCB | 1136 B |
| 链路线程 | 栈 3072 B + TCB | 3184 B |
| DHCP 线程 | 栈 3072 B + TCB | 3184 B |
| 内部 Modbus Master | 栈 4096 B + TCB | 4208 B |
| SPI2/I2C1 互斥锁 | 2 × Queue_t | 176 B |
| Modbus 配置/数据互斥锁 | 2 × Queue_t | 176 B |
| Modbus Master 信号量 | 1 × Queue_t | 88 B |
| MQTT TX 互斥锁及写信号量 | 2 × Queue_t | 176 B |
| 网络升级通知信号量 | 1 × Queue_t | 88 B |
| ETH RX 信号量 | 1 × Queue_t | 88 B |
| Modbus 请求队列 | Queue_t + 2 × 24 B | 136 B |
| Modbus 外部写队列 | Queue_t + 4 × 76 B | 392 B |
| Modbus 定时器 | 2 × Timer_t | 112 B |
| tcpip 邮箱 | Queue_t + 4 × 4 B 指针 | 104 B |
| tcpip 核心递归锁 | 1 × Queue_t | 88 B |
| netconn 邮箱及操作信号量 | 最多 4 × (104+88) B | 768 B |
| DNS 等待信号量 | MQTT/下载两个调用者最多 2 × Queue_t | 176 B |
| 分配块小计 | 35 个 live 块 | 19512 B |
| 堆首尾/初始对齐保留 | 保守留 16 B | 16 B |
| 不可拆分的块尾保留 | 每个 live 块至多计 16 B | 560 B |
| 模型合计 | 单次装配、当前调用图 | 20088 B |

五个动态线程的纯栈合计 16384 B。模块静态任务、存储各静态队列、就绪事件组及静态 Timer 命令队列不再加到 heap_4；LwIP 连接对象、PCB 和 pbuf 本体来自自己的池，其 RTOS 邮箱和信号量另计如上。

模型要求：按当前装配只初始化一次、连接/临时对象不超出实际池和调用者数量。块尾保留不代替外部碎片和最大空闲块检查。20 KiB 门槛比该模型高 392 B；24 KiB 比该模型高 4488 B，均为算术余额，不能称为实际可用最大块或历史最小空闲堆。

最大单次 heap_4 申请为 tcpip 的 5120 B 栈加 8 B 块头，即 5128 B；它在网络初始化初期创建。内部 Modbus Master 的 4104 B 栈块在 MQTT/下载任务启动前创建。不能拿装配完成后的 4488 B 算术余额反推前面的线程必定创建失败，也不能据此允许后续新增大线程。现有静态模块任务不再从该池申请整段栈。

本轮仅修正影响预算稳定性的生命周期：

- App_Entry 最多尝试五次，末尾直接判断最后结果。tcpip 只请求初始化一次，等待就绪最多 2 s；成功的 netif、链接和 DHCP 线程在后续重试中复用。正常无网线仍可初始化成功；整体初始化未完成时，链路等待返回 ETH_ERR_CONFIG。
- ethernetif_init 返回底层结果；PHY、收包信号量、接收线程或 HAL 启动失败不再记为成功。接收任务先创建、MAC 启动前不读取描述符；启动前失败删除本次任务/信号量，删除失败保留句柄。RX 池只初始化一次，成功后不因后置失败重置仍被引用的缓冲。
- MQTT 初始化部分失败回收 TX 锁，已初始化后不重复创建对象。发送、QoS1 pending 和连接删除共用原有 TX 锁；断连后关闭发送准入，删除失败保留连接，释放前不创建替代连接。
- 下载器用 netbuf_delete 同时释放 pbuf 和 netbuf；连接所有退出路径统一释放，失败保留最后引用，下一次下载先重试释放。原操作失败与清理失败分别记录，清理失败不能报告下载成功；过长 HTTP 请求在发送前拒绝，避免读取请求缓冲之外的内存。

此前生命周期修复增加普通 SRAM 链接占用24 B；用户随后将ucHeap减少8192 B。此前M2监督接入另增加24 B：启用发送超时后netconn池增16 B，任务句柄和连接完成标志增5 B，对齐增3 B；AppDebug Flash增2056 B。未增加动态任务、队列或缓冲容量。正常模型仍按最多四个 netconn 的 IPC 预算计数；被保留的失败连接占用其原槽位，不叠加创建。已有 RTOS 分配失败 Hook 的停机行为保持原样，不宣称所有内存不足都可继续运行。反复失败后的实际资源基线和碎片仍待测量。

2026-11-12真实暂停结算接入后，AppDebug普通SRAM另增8 B，Flash增2824 B；新增状态符号合计10 B，被原有对齐空隙抵消2 B。CCM、任务、队列和缓冲容量不变，FreeRTOS动态对象模型不变。四个应用预设构建、八个修改C单元严格编译/静态分析通过；41组任务/队列边界、8组协调顺序及4组段追加入口编译期模型通过，均不执行真实RTOS、网络或存储。完整运行验证仍待接口依赖与可信基线齐备。

### C 堆与密码库分配

当前 _sbrk 使用 [_end,_sstack)；AppDebug 的边界为 [0x2001CF68,0x2001FC00)，共11416 B；堆划分调整前为2264 B。对当前 newlib-nano 的只读反汇编确认，fresh 分配块至少使用 max(12,align4(n)+8) B，复用余块和对齐还可能增加占用。

| 对象/阶段 | 已确认的请求或容量 | 计数说明 |
| --- | ---: | --- |
| Modbus 常驻句柄及 4 站表 | 净 48+4×616=2512 B；fresh 块至少 56+2472=2528 B | 调整前2264 B至少缺264 B；当前总预算另见下表 |
| 解密工作区 | sizeof=1440 B，一次 calloc；fresh 块至少 1448 B | 操作结束清零并释放，不随固件整包大小增长 |
| GCM/AES 上下文 | 616 B / 280 B | 已嵌在上述工作区，不另算两份堆分配 |
| P-256 组与基点静态表 | 组结构 96 B，基点 comb 表在 Flash | 组结构在调用栈，静态常量/表不计入 C 堆 |
| 验签 restart 元数据 | ver=20、mul=36、muladd=52 B | 三类 calloc 元数据，不包含其 MPI limb 数据 |
| 普通点预计算表 | w=4，最多 8 × sizeof(point24)=192 B | 表内各坐标的 limb 另分配；基点静态表不再次计入 |
| 批量归一化元数据 | 最多 7 × sizeof(MPI8)=56 B | MPI limb 和逆元临时区另计 |
| MPI limb | 每 limb 4 B | grow 先分配新块，再清零释放旧块，存在短时并存 |
| C 库格式化 | Telemetry 实际使用 %.2f，链接含浮点格式化 | 不能由 512 B 输出缓冲推导内部动态分配上界 |

应用实际只启用 secp256r1，ECP_MAX_BITS=256、WINDOW_SIZE=4、FIXED_POINT_OPTIM=1、RESTARTABLE；NIST_OPTIM 未定义，相关 field 运算会走通用 MPI 归约，不能套用另一配置的快模内存数字。PLATFORM_MEMORY 未定义，mbedtls_calloc/free 使用 C 库分配器。

VerifyImage 的签名检查结束后才进入 GCM/明文摘要，签名函数清理 group/point/r/s/restart 后返回。两阶段临时空间不直接相加，但 Modbus 常驻表及其他任务的 C 库分配必须相加。仅 Modbus 与 GCM 的已知 fresh 分配块下限就是 2528+1448=3976 B，仍不是含 C 库并发的总峰值。

本轮按当前调用约束建立保守存活对象包络。newlib-nano 的块按 U(n)=max(12,align4(n)+8)+8 B 计数：前项包含请求、头部和返回指针对齐，末尾 8 B 覆盖无法拆分的余块（余量小于 12 B 且为 4 B 的倍数）。_end 已按 4 B 对齐。U 不包含对象之间尚未合并的外部空闲洞，不能直接当作 _sbrk 高水位。

验签同时只允许一个 UpgradeStage 修改操作。输入固定为 65 B 未压缩 P-256 公钥、32 B 摘要及两个 32 B 签名分量。基点 G 使用 Flash 常量表，普通公钥 Q 最多 8 个预计算点；ecp_muladd 传入 NULL 随机回调。字段值小于 P，常规运算后的 MPI 容量最多 16 个 4 B limb；通用除法内部的 X/Y/Z/T1 按最多 18/17/18/18 个 limb 计数，另计一次旧 17-limb 块与新块并存。模逆为当前 gcd_modinv_odd 实现，临时数组为 5×8×4=160 B，不套用其他版本的逆元算法。

| Q 点表预计算阶段的对象组 | 含管理开销的上界 |
| --- | ---: |
| 公钥/签名 MPI、e/s_inv、restart u1/u2 | 468 B |
| ver/mul/muladd 三个 restart 元数据块 | 156 B |
| 已完成的 u1×G 结果坐标 | 116 B |
| 8 个点的元数据数组 | 208 B |
| 点坐标：T0 按 8/8/1 limb，其余 7 点按每坐标 16 limb | 1796 B |
| 四个点运算临时 MPI 的 limb | 320 B |
| 7 个归一化 MPI 的元数据数组 | 72 B |
| 7 个归一化 MPI 及一个 t 的 limb，均按 16 limb | 640 B |
| 最深运算及扩容/缩容并存余量 | 432 B |
| 验签临时对象合计 | 4208 B |

上述各项取保守组合，部分对象不一定同时达到各自最大值。逐阶段对照的 comb 主循环包络为 3116 B；参数检查、标量准备、最终相加/归一化及 r 比较均被上述总包络覆盖。restart 只保留已有对象，工作函数返回时释放局部临时区；错误或取消清理不再新建密码工作区。两次明文检查也依次申请/释放同一类 GCM 工作区，不同时存活。

C 库按实际 ELF 分配调用点及 [newlib 4.5.0 官方源码](https://sourceware.org/pub/newlib/newlib-4.5.0.20241231.tar.gz) 对照。当前生产循环只有 Telemetry 执行浮点格式化，输入为 float32 提升至 double，格式固定 %.2f；Modbus 的相同测试调用被注释，未开启按任务独立 newlib reent。此范围内 dtoa 为 mode=3、precision=2，Bigint 档位 k 不超过 3；计入历史保留的 freelist 后，k=0/1/2/3 分别保守计 1/7/5/4 个块，另计 16 B mprec、132 B freelist 指针表和 24 B 随机数状态。按 U 合计 1100 B，不把 Bfree 当作归还 C 堆。上电 stdout 使用 _IONBF，固定缓冲 snprintf 不走 asprintf 扩容分支；本范围不计未调用的 strtod/asprintf 路径。

| 并发场景 | 对象预算（含管理开销） |
| --- | ---: |
| 常驻 Modbus 表及句柄 | 2544 B |
| 常驻 Modbus＋验签＋C 库缓存 | 2544+4208+1100=7852 B |
| 常驻 Modbus＋GCM＋C 库缓存 | 2544+1456+1100=5100 B |
| 调整前 C 堆相对上述最大对象预算的缺口 | 7852-2264=5588 B |
| 当前24 KiB候选的C堆跨度（已核对ELF） | 11416 B，已含RTOS堆调剂、回收Stage副本1016 B及结果交接增加24 B；对象预算未变 |
| 当前跨度扣除对象预算后剩余 | 11416-7852=3564 B |

调整前的5588 B是相对保守对象预算的容量缺口，不是实测缺口或缺口下限；此前已从RTOS预留调剂8192 B给C堆。7852 B不是实测峰值，当前3564 B是留给碎片及运行余量的预算，尚未证明足够，不能把未知外部碎片按0 B处理。当前证据支持以24 KiB配置继续验证，无需据此立即大幅删除核心功能；最终采用仍待运行证据。增加浮点格式/调用者、独立reent、其他曲线、并行认证、Config或HTTPS后须重算。

应用侧 app_memory.c 已实现 __malloc_lock/unlock，并显式接入 CMake。调度器启动前跳过加锁，任务中通过可嵌套的调度器挂起/恢复保护分配器，ISR 调用触发断言；不在钩子中分配、打印或等待外部事件，也不锁整个密码运算。最终 ELF 已确认两个强符号来自该文件，分别调用 vTaskSuspendAll/xTaskResumeAll。此保护不等于所有 C 库接口均已具备任务独立状态，也不能证明 C 堆容量够用；Boot 未修改。

### 候选准入与下一步

| 检查项 | 当前判断 |
| --- | --- |
| 链接/静态区 | 已核对，四预设构建通过；C堆跨度11416 B，实际运行仍待验证 |
| FreeRTOS 正常分配请求模型 | 20088 B，在 20 KiB 初始门槛内；外部碎片、错误清理和实板峰值未关闭 |
| C 堆对象并发预算 | 上界 7852 B，8 KiB 门槛余 340 B；含块内余量，不含外部碎片 |
| C 堆高水位及最大连续块 | 未验证，不能将对象预算当作总占用 |
| 24 KiB候选状态 | 已生成并构建，当前24576 B；最终运行准入待验证 |
| 本轮范围 | 核对持久Boot结果并交接MQTT PUBACK，复用原任务/队列，不改变业务容量 |

用户生成阶段的40文件哈希对照表明：只有.ioc与FreeRTOSConfig.h各一行32768→24576，278个USER CODE区域不变。该阶段四预设构建通过，AppDebug的443个普通SRAM有大小符号总量119308 B，仅ucHeap容量减少8192 B；CCM24个符号合计60828 B不变，启动任务栈仍4096 B，C分配锁仍实际链接。M2监督接入后重新核对四预设，M3Test占用不变；已知LwIP格式及MQTT未使用函数告警保留，无新增告警类型。

此前密码运算及浮点边界预算证据继续复用，算法、输入范围和格式化调用未变。本次三个修改C单元通过严格编译/静态分析，18组编译期控制流模型覆盖退避、连接成功/失败/超时、发送失败、迟到、任务归属和Telemetry等待；该模型不执行真实RTOS或网络调用。Windows应用控制阻止的本机C测试仍未执行，未进行实板验证。后续集中核对C/RTOS碎片、最大连续块、高水位及各任务栈；完整测量待统一权限、Config等剩余接口接齐后合并进行，不绕过监督取得通过结果。

应用 AES ROM 查表已经实施，不能再次计为未来收益。补传批量 10→4 可评估回收 9792 B CCM，但尚未实施，也不能直接抵扣普通 SRAM。任务栈、RX16/PBUF12、LwIP 10 KiB 和 4×16 业务容量本轮均不变。

完整 Config、HTTPS 的握手/X.509/缓冲、未来控制策略及运行峰值继续列为未关闭项；当前没有证明它们能容纳。后续运行测量须覆盖正常采样、双连接下载、重连、认证/释放、失败清理和栈高水位，并记录负载及持续时间。

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

**CMSIS-RTOS2 任务优先级数字越大越高**，与上面的 NVIC 规则不同。以下为2026-11-11合入Gitee bc38d97后的装配源码配置，不是实测峰值；接入后按完整调用链及栈高水位复核。

| 主责 / 用途 | 初始化 / 任务入口 | 任务优先级 | 静态栈 |
| --- | --- | --- | --- |
| M3 存储 | `Storage_Init` / `Storage_Task` | Normal，24 | 6KiB |
| M1 配置维护 | `Config_Init` / `Config_Task` | BelowNormal，16 | 512B |
| M1 Modbus | `Modbus_Init` / `vModbusPollTask` | AboveNormal，32 | 2KiB |
| M2 MQTT | `MqttClient_Init` / `MqttClient_Task` | Normal，24 | 6KiB |
| M2 遥测 | `Telemetry_Init` / `Telemetry_Task` | Normal，24 | 2560B |
| M4 监督 | `Monitor_Init` / `Monitor_Task` | AboveNormal，32 | 2KiB |
| M4 TF升级 | `UpgradeTf_Init` / `UpgradeTf_Task` | Normal，24 | 6KiB |
| M2 网络升级 | `NetworkUpgrade_Init` / `NetworkUpgrade_StartTask` | BelowNormal，16 | 6KiB，当前M2源码放CCM |
| 默认启动任务 | `StartDefaultTask` → `App_Entry` | Normal，24 | 4KiB |

Storage、Modbus、监督、TF、MQTT和Telemetry已绑定，Config仍为NULL；关闭GATEWAY_ENABLE_M2时不绑定MQTT/Telemetry，也不创建网络升级任务。七组模块栈的源码配置合计25KiB，另有M2网络升级6KiB及启动4KiB；实际保留量以map为准。Storage、Monitor和TF栈使用普通SRAM，其余现有模块栈及M2网络升级栈标记为CCM，不能将其缓冲直接交给DMA。默认启动任务装配完成后退出；监督任务在网络及业务初始化前启动。ETH接收/tcpip线程由网络适配创建，优先级约定为High（40）/AboveNormal（32），不在装配层重复创建。

初始化在调度器运行后准备本模块基础对象，耗时恢复放进所属任务。就绪状态通过 `app_readiness.h` 上报；初始化或任务创建成功不等于外设/业务就绪。存储队列、暂停/结算/同步和健康报告已接入；TF只授权本次按键操作，真实信任资料、网络权限及其他模块回调仍待齐备。

MQTT和Telemetry在各自任务入口登记健康：进展期限2 s，单次等待上限30 s。MQTT连接最多10 s、写入最多10 s、发送锁等待最多12 s；实际调用返回后才结束监督等待，退避分段检查原总期限。Telemetry自行覆盖启动、采集、序号和发送等待；网络升级任务与LwIP回调均不代报其健康。暂停请求每次清除旧确认，两个任务在业务安全点自行确认；MQTT还检查发送锁及旧补传应答。暂停期间保活、命令拒绝回执和升级状态继续发送。Storage_Task只收尾已经接纳的队列，写失败保留队首并锁存失败，不重试冒充成功；暂停结算需要回收段时返回忙且不推进序号。存储同步最后执行，恢复逆序且全部成功后才开放全局门控。源码已接入，真实并发和介质行为待实板验证。
