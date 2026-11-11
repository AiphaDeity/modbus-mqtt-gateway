# Modbus RTU–MQTT 双向协议转换网关

基于 STM32F407VET6 的嵌入式网关，用于连接 Modbus RTU 现场设备与 MQTT 平台。

## 规划功能

- Modbus RTU 主站采集与下行写控制。
- 以太网 MQTT 3.1.1 通信。
- TF 卡持久缓存、断网补传及业务回执确认。
- 本地配置、安全固件升级与串口恢复。
- Bootloader 与正常、回退及恢复应用统一看门狗监督和交接。

## 硬件平台

STM32F407VET6，配备 RS-485、LAN8720A 以太网、SDIO TF 卡、W25Q128JV SPI Flash 和 SD3078 RTC。

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
├─ firmware/app/                  应用工程
│  ├─ Core/                      应用入口、中断及初始化代码
│  ├─ Drivers/                   HAL 与 CMSIS，含组件许可证
│  ├─ Middlewares/               当前使用的 FreeRTOS 源码
│  ├─ cmake/                     应用构建配置及工具链文件
│  ├─ integration/               启动、任务装配及资源就绪协调
│  ├─ CMakeLists.txt
│  ├─ CMakePresets.json           应用工程预设
│  ├─ modbus-mqtt-gateway.ioc     CubeMX 配置
│  ├─ startup_stm32f407xx.s       启动文件
│  └─ STM32F407xx_FLASH.ld        链接脚本
├─ firmware/bootloader/           Boot、片内安装、回退及串口恢复
├─ firmware/shared/               Boot/App共用包认证、状态记录及看门狗
├─ tools/                         升级包制作和串口恢复工具
├─ docs/                         公共资源与基线验证说明
├─ .settings/                    ST 工程及工具包配置
├─ .vscode/                      VS Code 工程配置
├─ CMakeLists.txt                顶层构建入口
└─ CMakePresets.json             仓库统一构建预设
```

## 构建

使用 VS Code 和 ST 官方 STM32 扩展时：

1. 打开仓库根目录 `modbus-mqtt-gateway/`。
2. 按 `Ctrl + Shift + P` 打开命令面板，执行 `CMake: Select Configure Preset`，选择根目录工程的 `Debug` 预设。
3. 在命令面板执行 `CMake: Configure`，完成配置。
4. 在命令面板执行 `CMake: Build`，或按 `F7` 生成。

以上 `CMake:` 开头的选项属于 VS Code 命令面板，不是 PowerShell 命令。

Debug 固件输出位置：

```text
build/Debug/firmware/app/modbus-mqtt-gateway.elf
```

Boot 与偏移 App 使用以下预设，从仓库根目录选择并构建：

| 预设 | 用途 |
| --- | --- |
| Debug / Release | 从 `0x08000000` 独立启动的应用调试/优化构建 |
| AppDebug | 从 `0x08010000` 启动、由 Boot 管理的应用 |
| BootDebug | 64 KiB Boot，保留调试信息及启动诊断 |
| BootSizeCheck | 不含启动密码自测和测试信任根的完整 Boot 路径 |
| BootReceiveTest | 只读 Flash 一致性诊断，结束后停留 Boot，不接收升级包或跳转 App |
| BootRecoveryTest | 使用本机测试夹具的串口收包和恢复验证 |

Boot、App及配套工具统一使用持久兼容号2，其他版本拒绝，不自动迁移旧日志。首次部署及M3对接约定见[升级模块说明](firmware/app/services/upgrade/README.md)。

例如在根目录终端执行 `cube-cmake --preset AppDebug`，再执行 `cube-cmake --build --preset AppDebug`。Boot 测试预设需要本机自备 `.local/upgrade-fixture/boot_test_keys.h`，该夹具不随仓库分发。普通 Boot/App 的可信密钥映射通过 `GATEWAY_UPGRADE_TRUST_SOURCE` 接入；未配置时升级入口保持关闭。正常固件构建不依赖测试夹具。

调整目录或切换工具链后，如缓存仍引用旧路径，在命令面板执行 `CMake: Delete Build Directory and Reconfigure`，再执行生成。该操作清除所选构建目录，不影响源码。

使用 CubeMX 编辑 `firmware/app/modbus-mqtt-gateway.ioc`，代码生成位置保持在 `firmware/app/`。重新生成后检查构建配置差异，再从仓库根目录构建。

独立应用使用 `STM32F407xx_FLASH.ld`；AppDebug 使用 `STM32F407xx_APP.ld`，与 Boot 共同保留普通 SRAM 首 64 B 作为复位邮箱。堆、存储任务栈和监督任务栈使用普通 SRAM；其余业务栈的放置以 `integration/app_modules.c` 为准，CCM 中的缓冲不能交给 DMA。

模块接入时，在实际模块目录维护源码列表，并在 `firmware/app/CMakeLists.txt` 添加对应 `add_subdirectory(...)`。在 `integration/app_modules.c` 同时绑定模块的初始化函数和任务入口；任务由装配层统一创建。未接入模块保持两个入口均为 `NULL`，不提供返回成功的空实现。引脚、DMA 和中断配置以 `.ioc` 为准。

## 下载与调试

安装 Cortex-Debug 扩展和支持 STM32F4 的 OpenOCD，本配置按 ST OpenOCD 配套脚本检查。先按上文选择并生成 `Debug`，再按 F5 选择 `STM32F407 OpenOCD`；该调试配置使用 Debug ELF，不自动构建。

调试时按输入框填写本机路径（不加外层引号），无需把个人安装路径写入仓库：

| 输入 | 填写内容 |
| --- | --- |
| OpenOCD 程序 | `openocd.exe` 完整路径，或 PATH 中的 `openocd` |
| ST GCC bin | `gnu-tools-for-stm32/14.3.1+st.2/bin` 的实际目录，含 GDB、objdump、nm |
| OpenOCD 脚本目录 | 配套的 `scripts` 或 ST `st_scripts` 目录，内含 `interface/`、`target/`；它不一定和程序在同一目录 |
| SWD 探针 | DAPLink/CMSIS-DAP 或 ST-LINK；后者使用 ST 包中的 `interface/stlink-dap.cfg` |

当前速度为 1000kHz，`reset_config none` 使用软件复位。连接成功后停在 `main`；构建、实板启动及成员复现步骤见[公共基础验证](docs/baseline-check.md)。

## 项目文档

- [需求规格说明书](https://hcn4ticlccda.feishu.cn/docx/N1fadpRf2oYcRCxiR2KcFpfKntc)
- [系统设计说明书](https://hcn4ticlccda.feishu.cn/docx/JNMDdxoBio9f9bxxQPXcPINSnqh)
- [任务分工与 Git 协作说明](https://hcn4ticlccda.feishu.cn/docx/Q2EddAYPnoc9JOxRq3DcDtSXnZe)
