# Debug Session: selftest-silent

**Symptom**: 烧录含自检的固件后，串口只打印 `Gateway boot`（每次复位），但自检输出 `===== M3 SELF TEST =====` 从未出现。按复位可稳定复现 `Gateway boot`，说明程序入口可达但 Storage_Task 似乎没跑起来或在跑之前就崩了。

**Project Root**: `d:\Project\modbus-mqtt-gateway`
**Debug Evidence Dir**: `C:\Users\Administrator\AppData\Local\Temp\selftest-silent-evidence`

** hypotheses 列于下方"假设列表"章节，证据收集在插桩后更新。

## 假设列表（按可证伪性排序）

| # | 假设 | 观测点 | 类型 |
|---|------|--------|------|
| H1 | `Storage_Init()` 在创建 Storage_Task 之前就崩溃/HardFault 了 | 在 Storage_Init 开头加 printf，每步加 | 插桩验证 |
| H2 | `Storage_Init()` 成功但 `osThreadNew()` 返回 NULL（堆不够 / CB 不够） | 在 osThreadNew 调用后打印返回值 | 插桩验证 |
| H3 | Storage_Task 运行了但在进入 m3_selftest 之前的某步 init/call 里崩溃 | 在 Storage_Task 函数体顶部加 printf | 插桩验证 |
| H4 | Storage_Task 跑了且 m3_selftest 也调了，但 m3_selftest 内部先崩了 | 在 m3_selftest 开头加 printf | 插桩验证 |
| H5 | Printf/USART 在 Storage_Task 上下文里不可用（锁死/缓冲区问题） | 用 GPIO 翻转替代 printf 作为探针 | 可证伪性较弱 |

## 调试计划

1. 在 Storage_Init 开头和每个关键步骤前后插桩 printf ✅ 已完成
2. 在 Storage_Task 函数体顶部插桩 ✅ 已完成
3. 在 m3_selftest 开头插桩 —— 已有的 m3_selftest 自带 printf，且 Storage_Task 里加了 "about to run selftest" 探针
4. 编译 → 烧录 → 串口输出 → 判读
5. 根据证据定位崩溃位置 → 分析根因 → 修复 → 清理插桩

## 插桩位置（v1，2026-11-05）

storage.c 新增 12 个探针点：

| # | 探针 | 位置 |
|---|------|------|
| 1 | `[M3] Storage_Init enter` | Storage_Init 第一行 |
| 2 | `[M3] mutex=%p` | osMutexNew 之后 |
| 3 | `[M3] rtc init done` | rtc_sd3078_init 之后 |
| 4 | `[M3] spi flash ok=%d` | spi_flash_w25q128_init 之后 |
| 5 | `[M3] sdcard init done` | sdcard_tf_init 之后 |
| 6 | `[M3] seq init done` | storage_seq_init 之后 |
| 7 | `[M3] identity init done` | storage_identity_init 之后 |
| 8 | `[M3] timefloor ok=%d` | storage_timefloor_init 之后 |
| 9 | `[M3] ctrllog ok=%d` | storage_ctrllog_init 之后 |
| 10 | `[M3] Storage_Init exit ok` | Storage_Init return 前 |
| 11 | `[M3] Storage_Task enter` | Storage_Task 第一行 |
| 12 | `[M3] config open done` ~ `selftest returned` | Storage_Task 各步之间 |

## 判读规则

- 看到哪个探针、停在哪个探针 → 定位崩溃/阻塞在该探针**之后**的那行代码
- 只看到 `Gateway boot` 看不到 `[M3] Storage_Init enter` → 问题在 App_ModulesInit() 调用 Storage_Init 之前
- 看到 `Storage_Init exit ok` 但看不到 `Storage_Task enter` → 任务没创建成功或 defaultTask 崩了
