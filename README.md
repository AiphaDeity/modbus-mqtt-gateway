# Modbus RTU–MQTT 双向协议转换网关

基于 STM32F407VET6 的嵌入式网关，用于连接 Modbus RTU 现场设备与 MQTT 平台。

## 规划功能

- Modbus RTU 主站采集与下行写控制。
- 以太网 MQTT 3.1.1 通信。
- TF 卡持久缓存、断网补传及业务回执确认。
- 本地配置、安全固件升级与串口恢复。
- Bootloader 与正常、回退及恢复应用统一看门狗监督和交接。

## 硬件平台

STM32F407VET6，配备 RS-485、LAN8720A 以太网、SDIO TF 卡和 SD3078 RTC。当前外部存储使用 SPI3 连接的 W25Q64 模块（8 MiB），片选为 PD11。

引脚、DMA、中断、任务优先级与初始栈分配见[公共硬件与任务资源](docs/hardware-resources.md)。

## 开发环境

软件方案采用 HAL、FreeRTOS、LwIP 和 FatFs，使用 GCC + CMake + Ninja 构建。

| 工具 | 版本 |
| --- | --- |
| GCC 工具包 | ST `gnu-tools-for-stm32 14.3.1+st.2` |
| CMake | ST `4.4.0+st.1` |
| Ninja | ST `1.13.2+st.1` |
| STM32CubeF4 | `1.28.3` |

工具包由 ST 官方工具管理器安装，编辑器可自行选择。

## 目录结构

```text
modbus-mqtt-gateway/
├─ firmware/
│  ├─ app/                       应用工程
│  │  ├─ Core/                   CubeMX 入口、中断及初始化代码
│  │  ├─ Drivers/                HAL 与 CMSIS，含组件许可证
│  │  ├─ Middlewares/            FreeRTOS、LwIP、FatFs
│  │  ├─ FATFS/                  FatFs 生成配置与 SD 适配
│  │  ├─ bsp/                    以太网、RTC、TF、SPI Flash 驱动
│  │  ├─ common/                 应用共用配置定义与校验
│  │  ├─ integration/            启动、任务装配、监督与升级接入
│  │  ├─ services/               config、modbus、mqtt、storage、upgrade、network_upgrade
│  │  ├─ cmake/                  生成构建配置及工具链
│  │  ├─ modbus-mqtt-gateway.ioc  App CubeMX 配置
│  │  ├─ STM32F407xx_APP.ld       Boot 管理的 App 布局
│  │  └─ STM32F407xx_FLASH.ld     独立启动的 App 布局
│  ├─ bootloader/
│  │  ├─ Core/                   CubeMX 入口、中断及初始化代码
│  │  ├─ boot/                   包接收、认证、安装、回退及串口恢复
│  │  ├─ cmake/                  生成构建配置及工具链
│  │  ├─ bootloader.ioc          Boot CubeMX 配置
│  │  └─ STM32F407xx_FLASH.ld     Boot 与预留信任区布局
│  ├─ shared/                    共用协议、Flash 分区、状态、信任资料及看门狗
│  └─ third_party/mbedtls/        App/Boot 共用的密码库
├─ tools/                        制包、串口恢复、MQTT 及资源测量工具
├─ tests/                        团队共用测试数据
├─ docs/                         公共资源、技术说明及标明条件的历史报告
├─ build/                        构建产物，不入 Git
├─ .local/                       本地笔记、临时工具和实板证据，不入 Git
├─ .settings/                    ST 工程及工具包配置
├─ .vscode/                      VS Code 工程配置
├─ CMakeLists.txt                顶层构建入口
└─ CMakePresets.json             仓库统一构建预设
```

## 构建

使用 VS Code 和 ST 官方 STM32 扩展时：

1. 打开仓库根目录 `modbus-mqtt-gateway/`。
2. 按 `Ctrl + Shift + P` 打开命令面板，执行 `CMake: Select Configure Preset`，选择根目录工程的 `AppDebug` 预设。
3. 在命令面板执行 `CMake: Configure`，完成配置。
4. 在命令面板执行 `CMake: Build`，或按 `F7` 生成。
5. 需要配套 Boot 时，选择 `BootSizeCheck` 并重复配置、构建。

以上 `CMake:` 开头的选项属于 VS Code 命令面板，不是 PowerShell 命令。

当前配套 App 和 Boot 的输出位置：

```text
build/AppDebug/firmware/app/modbus-mqtt-gateway.elf
build/BootSizeCheck/firmware/bootloader/bootloader.elf
```

日常使用 `AppDebug` 和 `BootSizeCheck`。其余预设用于独立调试或专项测试，按需生成；删除构建目录不会删除预设。统一从仓库根目录选择并构建，不使用子工程的 CubeMX 默认预设：

| 预设 | 用途 |
| --- | --- |
| AppDebug | 从 `0x08010000` 启动、由 Boot 管理的应用 |
| BootSizeCheck | 不含启动密码自测和测试信任根的完整 Boot 路径 |
| Debug / Release / AppFlashDebug | 从 `0x08000000` 独立启动的应用；Release 使用体积优化 |
| BootDebug | 64 KiB Boot，保留调试信息及启动诊断 |
| BootReceiveTest | 只读 Flash 一致性诊断，结束后停留 Boot，不接收升级包或跳转 App |
| BootRecoveryTest | 使用本机测试夹具的串口收包和恢复验证 |
| M3Test | 禁用 M2 网络模块的独立 App 构建 |

Boot、App及配套工具统一使用持久兼容号2，其他版本拒绝，不自动迁移旧日志。首次部署及M3对接约定见[升级模块说明](firmware/app/services/upgrade/README.md)。

也可在仓库根目录的终端执行 `cube-cmake --preset AppDebug`，成功后执行 `cube-cmake --build --preset AppDebug`；构建 Boot 时将两条命令的预设都换为 `BootSizeCheck`。任一步报错时先处理错误，再继续。

Boot 测试预设需要本机自备 `.local/upgrade-fixture/boot_test_keys.h`，该夹具不随仓库分发。普通 Boot/App 默认通过 `firmware/shared/upgrade_trust_flash.c` 读取 Boot 末尾预留区的信任记录；缺少有效记录时升级入口保持关闭。`GATEWAY_UPGRADE_TRUST_SOURCE` 可指定其他保护区实现，布局和部署边界见[升级模块说明](firmware/app/services/upgrade/README.md#信任资料工具与启用)。正常固件构建不依赖测试夹具。

调整目录或切换工具链后，如缓存仍引用旧路径，在命令面板执行 `CMake: Delete Build Directory and Reconfigure`，再执行生成。该操作清除所选构建目录，不影响源码。

App 和 Boot 的 CubeMX 配置分别为 `firmware/app/modbus-mqtt-gateway.ioc`、`firmware/bootloader/bootloader.ioc`，生成位置保持在各自工程目录，并保留 USER CODE。自有业务源码放在 `services/`、`integration/` 或 `bootloader/boot/`，生成文件中只保留必要接入。重新生成后检查构建配置差异，再从仓库根目录构建。

独立应用使用 `STM32F407xx_FLASH.ld`；AppDebug 使用 `STM32F407xx_APP.ld`，与 Boot 共同保留普通 SRAM 首 64 B 作为复位邮箱。堆、存储任务栈和监督任务栈使用普通 SRAM；其余业务栈的放置以 `integration/app_modules.c` 为准，CCM 中的缓冲不能交给 DMA。

模块接入时，在实际模块目录维护源码列表，并在 `firmware/app/CMakeLists.txt` 添加对应 `add_subdirectory(...)`。在 `integration/app_modules.c` 同时绑定模块的初始化函数和任务入口；任务由装配层统一创建。未接入模块保持两个入口均为 `NULL`，不提供返回成功的空实现。引脚、DMA 和中断配置以 `.ioc` 为准。

## 下载与调试

安装 Cortex-Debug 扩展和支持 STM32F4 的 OpenOCD，本配置按 ST OpenOCD 配套脚本检查。先按上文构建 `BootSizeCheck` 和 `AppDebug`，再按 F5 选择 `STM32F407 OpenOCD`；该配置加载两份 ELF，使用 Boot ELF 的符号，不自动构建。首次升级部署还需匹配的信任资料和恢复建档，见[升级模块说明](firmware/app/services/upgrade/README.md)。

调试时按输入框填写本机路径（不加外层引号），无需把个人安装路径写入仓库：

| 输入 | 填写内容 |
| --- | --- |
| OpenOCD 程序 | `openocd.exe` 完整路径，或 PATH 中的 `openocd` |
| ST GCC bin | `gnu-tools-for-stm32/14.3.1+st.2/bin` 的实际目录，含 GDB、objdump、nm |
| OpenOCD 脚本目录 | 配套的 `scripts` 或 ST `st_scripts` 目录，内含 `interface/`、`target/`；它不一定和程序在同一目录 |
| SWD 探针 | DAPLink/CMSIS-DAP 或 ST-LINK；后者使用 ST 包中的 `interface/stlink-dap.cfg` |

当前 SWD 速度为 1000 kHz，`reset_config none` 使用软件复位，启动调试后停在 Boot 的 `main`。构建通过仅证明编译和链接成功，实际启动、业务及升级结果仍以板端验证为准。

## 项目文档

- [硬件与运行资源](docs/hardware-resources.md)：引脚、Flash 分区、任务与当前内存占用。
- [升级接口与恢复操作](firmware/app/services/upgrade/README.md)：包认证、持久格式、安装回退及维护协议。
- [Modbus 接口与 TF 配置](firmware/app/services/modbus/Modbus_Interface_Guide.md)：采集接口、配置文件和当前控制边界。
- [需求规格说明书](https://hcn4ticlccda.feishu.cn/docx/N1fadpRf2oYcRCxiR2KcFpfKntc)
- [系统设计说明书](https://hcn4ticlccda.feishu.cn/docx/JNMDdxoBio9f9bxxQPXcPINSnqh)
- [任务分工与 Git 协作说明](https://hcn4ticlccda.feishu.cn/docx/Q2EddAYPnoc9JOxRq3DcDtSXnZe)
