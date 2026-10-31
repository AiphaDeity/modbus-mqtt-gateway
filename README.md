# Modbus RTU–MQTT 双向协议转换网关

基于 STM32F407VET6 的嵌入式网关，用于连接 Modbus RTU 现场设备与 MQTT 平台。

## 功能

- Modbus RTU 主站采集与下行写控制。
- 以太网 MQTT 3.1.1 通信。
- TF 卡持久缓存、断网补传及业务回执确认。
- 本地配置、安全固件升级与串口恢复。

## 硬件平台

STM32F407VET6，配备 RS-485、LAN8720A 以太网、SDIO TF 卡、W25Q128JV SPI Flash 和 SD3078 RTC。

## 开发环境

软件采用 HAL、FreeRTOS、LwIP 和 FatFs，使用 GCC + CMake + Ninja 构建。

| 工具 | 版本 |
| --- | --- |
| GCC 工具包 | ST `gnu-tools-for-stm32 14.3.1+st.2` |
| CMake | ST `4.4.0+st.1` |
| Ninja | ST `1.13.2+st.1` |

工具包由 ST 官方工具管理器安装，编辑器可自行选择。

Windows 下检查工具环境：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\check-environment.ps1
```

自定义工具包目录时，使用脚本参数 `-BundlesRoot <目录>`。

## 项目文档

- [需求规格说明书](https://hcn4ticlccda.feishu.cn/docx/N1fadpRf2oYcRCxiR2KcFpfKntc)
- [系统设计说明书](https://hcn4ticlccda.feishu.cn/docx/JNMDdxoBio9f9bxxQPXcPINSnqh)
- [任务分工与 Git 协作说明](https://hcn4ticlccda.feishu.cn/docx/Q2EddAYPnoc9JOxRq3DcDtSXnZe)
