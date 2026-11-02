# 公共基础验证

本清单验证共同工程能构建、启动及进入 FreeRTOS。各业务驱动、Bootloader 和整机验收另行开展。

## 本机检查

2026-11-02，STM32F407VET6，ST GCC `14.3.1+st.2`、CMake `4.4.0+st.1`、Ninja `1.13.2+st.1`，Debug。来源为当前工作区（基于 `09161ae`，含未提交修改），不是已发布版本。

| 检查 | 结果 |
| --- | --- |
| 工具版本核对 | 通过，与 README 工具版本表一致 |
| 从空构建目录编译链接 | 通过，独立目录 `build/Baseline`，54 步编译链接完成，退出码 0 |
| ELF 向量表、任务栈位置 | 通过，向量表 `0x08000000`；五个业务栈均在普通 SRAM 且 8 字节对齐 |
| 调试配置格式及 OpenOCD 脚本加载 | 通过，按 Cortex-Debug 1.12.1 检查；CMSIS-DAP、ST-LINK 原生 DAP 脚本均可加载退出，未连接探针/目标板 |
| 最新精简固件实板启动、RTOS 时基 | 待 M4 在板上验证 |
| 其他成员本机构建 | 待成员取得相同版本后各自验证 |

本次 ELF：RAM 41,736B/128KiB（31.84%），CCM 0B/64KiB，Flash 38,304B/512KiB（7.31%）。业务栈、启动栈和 FreeRTOS 固定堆已包含在链接占用中；链接余量不是运行期峰值。

核对产物为 `build/Baseline/firmware/app/modbus-mqtt-gateway.elf`，SHA-256 为 `606482bcc3a900c47257f9dda0c42da5bcaf0786bf8cee1e497d4bc0f30f2b43`。此目录用于独立构建检查；F5 仍使用常规 `build/Debug`，下载前按 README 重新生成 Debug，不手改调试路径指向检查目录。

## 实板检查步骤

1. 按 README 构建根目录 `Debug`，使用本次生成的 `build/Debug/firmware/app/modbus-mqtt-gateway.elf`。探针接 SWDIO、SWCLK、GND，板子供电；可选接 NRST，当前配置使用软件复位。
2. CH340 的 RX 接 PA9、GND 接板上 GND，串口监视器设为 115200、8N1。板子自行供电，CH340 的电源输出不接板子。串口号以本机识别结果为准。
3. 按 F5，选择本机工具路径及实际探针；确认调试停在 `main`。在 `App_Entry` 的 `App_ModulesStart()` 后、`osThreadExit()` 前设断点，再继续。
4. 到达该断点时，检查 `readiness_events` 非空、`modules_initialized` 与 `modules_started` 为真；`module_slots` 的五个状态应为 `APP_MODULE_TASK_NOT_CONNECTED`，当前没有业务模块是预期状态。
5. 移除断点并继续，串口应出现 `Gateway boot`。暂停记录 `uwTick` 和 `xTickCount`，继续运行数秒后再暂停，两者应前进；任务可停在 FreeRTOS Idle 路径，不应落在 `Error_Handler` 或 HardFault。

仅看到启动打印不能证明 RTOS 启动。暂停后的 `SIGINT` 是调试器暂停信号；结合断点、时基和实际调用栈判断，不把它单独当作故障。

## 成员复现与交付

成员取得相同代码版本后，对照 README 核对工具版本、配置根目录 `Debug`、构建 ELF；有板者按上面的步骤检查。记录代码版本、工具版本、构建结果、板卡/探针及异常日志；没有板时明确只完成构建。

作者核对差异及暂存内容后，按团队现有分支流程提交/合入共同基线，再由成员同步该提交。本清单不会因单人构建通过而自动标记四人复现或实板通过。
