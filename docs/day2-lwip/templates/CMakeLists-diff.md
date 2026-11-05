# CMake / 配置改动清单（Day2 带 OS 版 · 基于同事重构后的新工程）

对照 M2 工程现状（feature/day2-lwip 分支）逐条改。改完全部后再编译。

【前提（已由同事/我完成，勿重复）】
  - stm32f4xx_hal_conf.h 已开 HAL_ETH_MODULE_ENABLED / HAL_UART_MODULE_ENABLED（同事已开）
  - stm32cubemx/CMakeLists.txt 已含 hal_eth.c / hal_uart.c / FreeRTOS 全套（同事已接）
  - Middlewares/LwIP/src（协议栈 305 文件）已拷入工程（已就位）
  - Middlewares/LwIP/CMakeLists.txt（lwip 库，含 api 层 + 链接 stm32cubemx）已写好（已就位）

---

## 改动 1：顶层 CMakeLists.txt 接入 LwIP 库（firmware/app/CMakeLists.txt）

在 `add_subdirectory(cmake/stm32cubemx)` 之后（第 32 行附近）加一行：

```cmake
add_subdirectory(Middlewares/LwIP)
```

在 target_link_libraries 里（`stm32cubemx` 之后）补两个库：

```cmake
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    lwip            # ← 加
    m2_ethernet     # ← 加
)
```

> 说明：同事顶层原来只链接 stm32cubemx；bsp 子目录通过 add_subdirectory(bsp) 引入。

---

## 改动 2：bsp/CMakeLists.txt 加 ethernet 子目录（firmware/app/bsp/CMakeLists.txt）

同事的 bsp/CMakeLists.txt 只接了 rs485，追加一行：

```cmake
add_subdirectory(ethernet)
```

---

## 改动 3：bsp/ethernet/ 目录新建（当前只有 .gitkeep 占位）

放入/新建以下文件（内容见同目录模板）：

| 文件 | 来源 | 动作 |
|---|---|---|
| ethernet.h / ethernet.c | 模板（带 OS 版） | 手敲 |
| ethernetif.h / ethernetif.c | 模板（带 OS 版） | 手敲 |
| lwipopts.h | 模板（带 OS 版） | 手敲 |
| sys_arch.c | 探索者例程 `arch/sys_arch.c` | 原样拷贝（FreeRTOS 适配层，一行不改） |
| CMakeLists.txt | 模板 `bsp_ethernet_CMakeLists.txt` | 新建（内容即模板全文） |

sys_arch.c 拷贝源：
`C:\Users\69588\AppData\Local\Temp\atk_f407\1_Example\SPL\lwip\MiddleWare\lwip-2.2.0\arch\sys_arch.c`

> sys_arch.c 提供：sys_sem_new/sys_mbox_new/sys_thread_new/sys_now 等 lwip 需要的
> FreeRTOS 适配函数（带 OS 模式协议栈必用，例程原版可直接用，库无关）。

---

## 改动 4：main.c 加一行调用（见 main-c-改动.txt）

USER CODE 2 区 `printf("Gateway boot\r\n"); fflush(stdout);` 之后加：

```c
m2_ethernet_init();
```

---

## 改完后的目录结构（firmware/app 下）

```
firmware/app/
├── CMakeLists.txt                    ← 改（加 Middlewares/LwIP + lwip/m2_ethernet 链接）
├── Core/Src/main.c                   ← 改（USER CODE 2 区加 m2_ethernet_init()）
├── Middlewares/LwIP/                 ← 已就位（src 305 文件 + CMakeLists.txt）
│   ├── CMakeLists.txt
│   └── src/...
├── bsp/
│   ├── CMakeLists.txt                ← 改（加 add_subdirectory(ethernet)）
│   ├── ethernet/                     ← 新建（.gitkeep 删除或保留均可）
│   │   ├── CMakeLists.txt            ← 新建
│   │   ├── ethernet.c / ethernet.h   ← 手敲
│   │   ├── ethernetif.c / ethernetif.h ← 手敲
│   │   ├── lwipopts.h                ← 手敲
│   │   └── sys_arch.c                ← 拷贝例程
│   └── rs485/                        ← 同事的，不动
└── cmake/stm32cubemx/CMakeLists.txt  ← 同事的，不动
```

## 编译验证

VS Code 打开 `firmware/app` → Ctrl+B。
预期通过；若报错，99% 是下面几类（对照排查）：
1. `cannot open source file "lwip/opt.h"` → 改动 1 的 lwip 库没接入/没链接
2. `unknown type name 'osSemaphoreId_t'` → ethernetif.c 没 include "cmsis_os.h"
   或 m2_ethernet 没链接 stm32cubemx（FreeRTOS/CMSIS 头没传进来）
3. `undefined reference to 'sys_sem_new'/'sys_now'` → sys_arch.c 没拷贝或没加进 CMakeLists
4. `duplicate definition of 'HAL_ETH_MspInit'` → 你在 ethernetif.c 里又写了 MspInit
   （同事 eth.c 已有，删掉 ethernetif.c 里的）
5. `duplicate definition of '__io_putchar'` → 你在 main.c 又写了重定向（同事已有，不要写）
