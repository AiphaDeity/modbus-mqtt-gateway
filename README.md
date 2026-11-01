# Modbus RTU–MQTT 双向协议转换网关

基于 STM32F407VET6 的嵌入式网关，用于连接 Modbus RTU 现场设备与 MQTT 平台。

## 规划功能

- Modbus RTU 主站采集与下行写控制。
- 以太网 MQTT 3.1.1 通信。
- TF 卡持久缓存、断网补传及业务回执确认。
- 本地配置、安全固件升级与串口恢复。

## 硬件平台

STM32F407VET6，配备 RS-485、LAN8720A 以太网、SDIO TF 卡、W25Q128JV SPI Flash 和 SD3078 RTC。

## 开发环境

软件方案采用 HAL、FreeRTOS、LwIP 和 FatFs，使用 GCC + CMake + Ninja 构建。

| 工具 | 版本 |
| --- | --- |
| GCC 工具包 | ST `gnu-tools-for-stm32 14.3.1+st.2` |
| CMake | ST `4.4.0+st.1` |
| Ninja | ST `1.13.2+st.1` |
| STM32CubeF4 | `1.28.3` |

工具包由 ST 官方工具管理器安装，编辑器可自行选择。

Windows 下检查工具环境：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\check-environment.ps1
```

自定义工具包目录时，使用脚本参数 `-BundlesRoot <目录>`。

## 目录结构

```text
modbus-mqtt-gateway/
├─ firmware/app/                  应用工程
│  ├─ Core/                      应用入口、中断及初始化代码
│  ├─ Drivers/                   HAL 与 CMSIS，含组件许可证
│  ├─ cmake/                     应用构建配置及工具链文件
│  ├─ CMakeLists.txt
│  ├─ CMakePresets.json           应用工程预设
│  ├─ modbus-mqtt-gateway.ioc     CubeMX 配置
│  ├─ startup_stm32f407xx.s       启动文件
│  └─ STM32F407xx_FLASH.ld        链接脚本
├─ tools/                        主机侧工具与环境检查脚本
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

调整目录或切换工具链后，如缓存仍引用旧路径，在命令面板执行 `CMake: Delete Build Directory and Reconfigure`，再执行生成。该操作清除所选构建目录，不影响源码。

使用 CubeMX 编辑 `firmware/app/modbus-mqtt-gateway.ioc`，代码生成位置保持在 `firmware/app/`。重新生成后检查构建配置差异，再从仓库根目录构建。

当前最小应用已通过迁移后的构建验证，板卡下载和运行尚待验证。

## 项目文档

- [需求规格说明书](https://hcn4ticlccda.feishu.cn/docx/N1fadpRf2oYcRCxiR2KcFpfKntc)
- [系统设计说明书](https://hcn4ticlccda.feishu.cn/docx/JNMDdxoBio9f9bxxQPXcPINSnqh)
- [任务分工与 Git 协作说明](https://hcn4ticlccda.feishu.cn/docx/Q2EddAYPnoc9JOxRq3DcDtSXnZe)
