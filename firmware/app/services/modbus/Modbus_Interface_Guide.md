# Modbus 模块对外接口文档

> 版本：v2.0
> 适用对象：**MQTT 数据上报方**、**Web/平台配置方**、**持久化存储方**
> 对外头文件：`#include "mb_api.h"`（点位相关另加 `#include "mb_pointmap.h"`）

---

## 目录

- [1. 模块职责与文件结构](#1-模块职责与文件结构)
- [整体流程图（上电 → 运行闭环）](#整体流程图上电--运行闭环)
- [2. 三类使用方速查](#2-三类使用方速查)
- [3. 数据结构](#3-数据结构)
- [4. 点位表配置](#4-点位表配置)
- [5. 上报模式：RAW / PARSED / BOTH](#5-上报模式raw--parsed--both)
- [6. MQTT 上报方：pull 接口用法](#6-mqtt-上报方pull-接口用法)
- [7. 写操作：FC6 / FC16](#7-写操作fc6--fc16)
- [8. 从站增删改 + 上电加载](#8-从站增删改--上电加载)
- [9. 持久化接口（仅声明，未实现）](#9-持久化接口仅声明未实现)
- [10. 锁规则与死锁清单](#10-锁规则与死锁清单)
- [11. 错误码](#11-错误码)
- [12. 日志等级](#12-日志等级)
- [13. FAQ](#13-faq)

---

## 1. 模块职责与文件结构

```
services/modbus/
├── modbus.h / modbus.c        移植库（ModbusQuery / ModbusQueryInject / CRC）
├── UARTCallback.c             串口收发回调，把应答帧送回移植库
├── ModbusConfig.h
│
├── mb_api.h               ★ 唯一对外头文件（外部模块只 include 这个）
├── mb_internal.h              内部头，.c 专用（extern mb_handle / hModbusMaster）
│
├── mb_poll.c                  状态机 + 硬件初始化 + poll 任务（无 malloc）
├── mb_slave.c                 句柄分配 + 从站 CRUD + 配置回调（唯一 malloc/free）
├── mb_api.c                   pull 接口：遍历 / 快照 / 注入写
│
├── mb_pointmap.h/.c           点位表类型 + 换算（已实现）
└── mb_pointmap_flash.h        持久化接口（只有声明，实现由存储方提供）
```

职责边界（改动时不要越界）：

| 文件 | 负责 | 不负责 |
|------|------|--------|
| `mb_poll.c` | 单槽一问一答状态机、RS485 收发、阶梯冷却、采集周期 | malloc、从站增删、加配置锁 |
| `mb_slave.c` | 句柄初始化、`malloc/free`、增删从站、触发配置回调 | 发起 Modbus 请求 |
| `mb_api.c` | 对外 pull 接口、写请求入队 | 状态机推进 |
| `mb_pointmap.c` | 寄存器值 → 物理量换算、点位表读写 | 持久化 I/O |

数据流总览：

```
  RS485 从站
      │  应答帧
      ▼
┌─────────────────────────────────────────────┐
│ mb_poll.c  poll 任务（硬实时，单槽状态机）   │
│  ERR_OK_QUERY 分支：                         │
│    reg_data_buf[] ← 应答寄存器               │
│    valid_point    ← 本次读取数量             │
│    data_valid = 1      is_online = 1         │
│    ★ 只存原始值，不做任何换算                │
└─────────────────────────────────────────────┘
      │  reg_data_buf[]（xSlaveDataMutex 保护）
      ▼
┌─────────────────────────────────────────────┐
│ 外部任务（MQTT / Web）pull 模式               │
│  mb_take_slave_snapshot()  → 原始寄存器       │
│  mb_pointmap_parse_slave() → 物理量（换算）   │
└─────────────────────────────────────────────┘
```

> 换算不在 poll 任务里做，也不预存进 `mb_slave_runtime_t`。原始寄存器与解析结果两条路径，由调用方按上报模式自行选择。

---

## 整体流程图（上电 → 运行闭环）

```
═══════════════════════════════ ① 上电初始化 ═══════════════════════════════

  main()
   │
   ├─ Modbus_Init() ──────────────┐
   │    ├─ ModbusInit()            │  移植库资源（队列/信号量）
   │    ├─ mb_handle_init()        │  malloc 句柄 + 两把锁 + 写队列
   │    └─ ModbusStart()           │  开 UART DMA 接收
   │                               │
   ├─ mb_set_config_change_cb(on_cfg_change)   ← 注册存储方"门铃"
   │
   ├─ mb_config_batch_begin() ──────────┐
   │    for 每个存储槽位:                │  抑制所有 ADD 回调
   │      mb_apply_slave_config(UPSERT,  │
   │                            id,      │  cfg → telegram
   │                            &cfg,    │  points → points[]
   │                            points)  │
   ├─ mb_config_batch_end() ────────────┘  只触发 1 次 RELOAD 回调
   │
   └─ xTaskCreate(vModbusPollTask) / MQTT 任务 / 存储任务

═══════════════════════════════ ② 运行期：三条并行流 ═══════════════════════

 流 A · 采集流（poll 任务，硬实时）          流 B · 上报流（MQTT 任务，周期 pull）
 ┌──────────────────────────────┐          ┌──────────────────────────────┐
 │ IDLE: 优先取写队列            │          │ mb_for_each_valid_slave()    │
 │   └→ ModbusQueryInject       │          │   收集 valid 从站 id          │
 │      否则按采集周期选从站      │          │        │                     │
 │ TX:   ModbusQuery 发请求帧    │          │        ▼ 每个 id              │
 │ WAIT: 阻塞等应答              │          │ mb_take_slave_snapshot(id)   │
 │   ├─ ERR_OK ─────────────────┼────┐     │   ├─ data_valid=0 → 跳过     │
 │   │  持 xSlaveDataMutex      │    │     │   └─ data[] = 原始寄存器     │
 │   │  reg_data_buf ← 寄存器    │    │     │        │                     │
 │   │  data_valid=1            │    │     │        ▼ 查上报模式           │
 │   └─ TIMEOUT → 阶梯冷却       │    │     │   RAW:   直接用 snap.data[] │
 └──────────────────────────────┘    │     │   PARSED:                    │
                                      │     │   mb_pointmap_parse_slave() │
              RAM（slave_list） ◀─────┘     │     ★ 解析在这里发生:        │
        ┌─────────────────────────┐         │     raw×scale+offset        │
        │ slave_list[i]           │ ◀───────┤        │                     │
        │  ├─ telegram            │  读      │        ▼                     │
        │  ├─ reg_data_buf[32]    │         │  MQTTPublish(topic, json)   │
        │  ├─ points[32]          │         └──────────────────────────────┘
        │  └─ data_valid/...      │
        └─────────────────────────┘
                      ▲ 写
 流 C · 配置流（MQTT 下发 / Web）           流 D · 持久化流（存储任务）
 ┌──────────────────────────────┐          ┌──────────────────────────────┐
 │ 平台 JSON → 解析              │          │ on_cfg_change(reason, id):   │
 │   │                          │  回调     │   s_cfg_dirty = 1  ← 只置标志│
 │   ▼                          │ ─────────▶│        │                     │
 │ mb_apply_slave_config()      │  1 次     │        ▼ 500ms 周期           │
 │   ├─ UPSERT: telegram+points │          │  vStorageTask:               │
 │   ├─ 只改 points             │          │   if (dirty)                 │
 │   └─ DELETE: valid=0         │          │     mb_pointmap_flash_save() │
 │   （多次内部变更合并为        │          │     └─ 遍历 slave_list       │
 │      一次回调）               │          │        整体快照+CRC → Flash   │
 └──────────────────────────────┘          └──────────────────────────────┘

═══════════════════════════════ ③ 掉电 → 再上电：回到 ① ═══════════════════
```

**四个角色各记一句话**：

| 角色 | 只做 |
|------|------|
| poll 任务 | 采集原始寄存器进 `reg_data_buf`，置 `data_valid` |
| MQTT 任务 | 周期 pull：快照拿原始值 / `parse_slave` 拿物理量 |
| 配置方（MQTT/Web） | 一律走 `mb_apply_slave_config`，写 RAM + 触发回调 |
| 存储任务 | 回调置脏标志 → 周期合并写 Flash |

---

## 2. 三类使用方速查

### 2.1 MQTT 上报方

| 函数 | 作用 |
|------|------|
| `mb_for_each_valid_slave(cb, ctx)` | 遍历所有 `valid && enable` 的从站（**只建议用来收集 ID**，见第 6 节） |
| `mb_take_slave_snapshot(id, &snap)` | 取原始寄存器快照（含 `data_valid` 新鲜度） |
| `mb_pointmap_parse_slave(id, out)` | 按点位表换算成物理量 |
| `mb_pointmap_get_report_mode()` | 取全局上报模式 |
| `mb_inject_write(...)` | 下发写操作：FC5/15 写线圈，FC6/16 写寄存器 |

### 2.2 Web / 平台配置方

| 函数 | 作用 |
|------|------|
| `mb_pointmap_set_slave_points(id, points)` | 下发某个从站的点位表（写入即生效） |
| `mb_pointmap_get_slave_points(id, out)` | 读取当前点位表（Web 表单回显） |
| `mb_pointmap_set_report_mode(mode)` | 切换 RAW / PARSED / BOTH |
| `mb_add_slave(&cfg)` / `mb_delete_slave(id)` | 从站本身增删改 |

### 2.3 持久化存储方

| 函数 | 作用 |
|------|------|
| `mb_set_config_change_cb(cb, ctx)` | 注册配置变更回调 |
| `mb_handle_get()` | 拿全局句柄，遍历 `slave_list` 取配置 |
| `mb_pointmap_flash_save()` / `_load()` | **仅声明**，实现由存储方自己写（见第 9 节） |
| `mb_notify_reload_complete()` | 上电批量加载结束的收尾通知 |

---

## 3. 数据结构

### 3.1 从站运行时结构

```c
struct mb_slave_runtime
{
    uint8_t  valid;                               // 1=有效；0=空位
    modbus_t telegram;                            // 协议配置（见 3.2）

    uint32_t last_trigger_tick;                   // 上次触发采集的 tick
    uint8_t  fail_count;                          // 本轮已重试次数
    uint8_t  consecutive_fail;                    // 连续失败次数（阶梯冷却）

    uint16_t reg_data_buf[MB_REG_MAX_PER_SLAVE];  // ★ 原始寄存器缓存（32 个）
    uint16_t valid_point;                         // ★ 本次有效的寄存器数
    uint8_t  is_online;                           // ★ 0/1
    mb_errot_t collect_status;                    // 上次采集结果（ERR_OK_QUERY 等）
    uint8_t  data_valid;                          // ★ 1=有新鲜数据

    mb_point_t points[MB_POINTS_PER_SLAVE];       // ★ 点位表（32 个）
};
```

「★」标记的是外部真正要用的字段。

### 3.2 telegram（`modbus_t`）里需要关心的字段

```c
u8id                 从站地址 1~247
u8fct                功能码：3=读保持寄存器，4=读输入寄存器
u16RegAdd            寄存器起始地址（Modbus 协议地址，不是数组下标）
u16CoilsNo           本次读取数量，≤ MB_REG_MAX_PER_SLAVE(32)
u16reg               指向 reg_data_buf，由框架自动设置，外部不要改
collect_interval_ms  采集周期（ms）
enable               是否参与轮询
retry_cnt            本轮失败重试次数
```

### 3.3 数据快照

```c
typedef struct
{
    uint8_t  slave_id;                      // 从站地址
    uint8_t  func_code;                     // ★ 功能码（FC01/02=data[] 是线圈打包字；FC03/04=寄存器值）
    uint8_t  is_online;                     // 在线状态
    uint8_t  data_valid;                    // ★ 1=本次周期 poll 成功；0=无新数据
    uint16_t reg_addr;                      // 寄存器起始地址
    uint16_t valid_point;                   // 有效长度（FC01/02=打包字数 ceil(N/16)；FC03/04=寄存器数 N）
    uint16_t data[MB_REG_MAX_PER_SLAVE];    // 原始寄存器/打包字拷贝

    uint32_t   last_collect_tick;           // 最后一次采集（成功或失败）的 tick
    mb_errot_t collect_status;              // 最后一次采集结果（ERR_OK_QUERY 等）
} mb_slave_snapshot_t;
```

> RAW 模式下先查 `func_code`：FC01/02 时 `data[]` 是打包字（位序见 4.1.1），不要当寄存器值直接发。

### 3.4 点位配置 / 解析结果

```c
#define MB_POINT_NAME_MAX    16
#define MB_POINT_UNIT_MAX     8
#define MB_POINTS_PER_SLAVE  32

typedef enum {
    MB_DT_UINT16  = 0,   // 1 个寄存器
    MB_DT_INT16   = 1,   // 1 个寄存器
    MB_DT_UINT32  = 2,   // 2 个寄存器（大端拼接）
    MB_DT_INT32   = 3,   // 2 个寄存器
    MB_DT_FLOAT32 = 4,   // 2 个寄存器（IEEE754）
    MB_DT_BIT     = 5,   // 1 个寄存器（位提取，配 bit_index）
    MB_DT_MAX
} mb_data_type_t;

typedef struct {
    uint16_t       reg_index;                    // ★ reg_data_buf[] 的下标（不是 Modbus 地址）
    mb_data_type_t data_type;
    float          scale;                        // 系数
    float          offset;                       // 偏移
    char           name[MB_POINT_NAME_MAX];      // JSON key
    char           unit[MB_POINT_UNIT_MAX];      // 单位
    uint8_t        valid;                        // 1=有效点位；0=空位
    uint8_t        bit_index;                    // 仅 MB_DT_BIT 用：位号 0~15
} mb_point_t;

typedef struct {
    char           name[MB_POINT_NAME_MAX];
    char           unit[MB_POINT_UNIT_MAX];
    float          value;                        // 换算后的物理值
    mb_data_type_t raw_type;
    uint8_t        valid;
} mb_parsed_point_t;
```

### 3.5 配置变更回调

```c
typedef enum {
    MB_CFG_CHANGE_ADD     = 0,   // mb_add_slave 新增
    MB_CFG_CHANGE_UPDATE  = 1,   // mb_add_slave 更新已有 / mb_pointmap_set_slave_points 点位表变更
    MB_CFG_CHANGE_DELETE  = 2,   // mb_delete_slave
    MB_CFG_CHANGE_RELOAD  = 3,   // （保留，建议外部区分）
    MB_CFG_CHANGE_GLOBAL  = 4,   // ★ 全局配置变更（批量加载结束、上报模式切换等，slave_id=0）
} mb_cfg_change_reason_t;

typedef void (*mb_cfg_change_cb_t)(mb_cfg_change_reason_t reason,
                                    uint8_t slave_id,      // 全局类变更时为 0
                                    void *user_ctx);
```

---

## 4. 点位表配置

### 4.1 换算规则

统一公式，所有类型一致：

```
physical = raw * scale + offset
```

`raw` 的取法：

| 类型 | 寄存器数 | raw 计算 |
|------|---------|---------|
| `MB_DT_UINT16` | 1 | `reg[i]` |
| `MB_DT_INT16` | 1 | `(int16_t)reg[i]` |
| `MB_DT_UINT32` | 2 | `((uint32_t)reg[i] << 16) | reg[i+1]` |
| `MB_DT_INT32` | 2 | `(int32_t)(((uint32_t)reg[i] << 16) | reg[i+1])` |
| `MB_DT_FLOAT32` | 2 | `memcpy` 到大端 uint32 后按 IEEE754 解释 |
| `MB_DT_BIT` | 1 | `(reg[i] >> bit_index) & 1` —— 位提取，配 `bit_index` 字段（0~15） |

- 多寄存器**严格大端**（高字在前），符合 Modbus RTU 规范。
- `reg_index + 寄存器数 > valid_point` 时该点位返回 0，不越界读。
- `data_type >= MB_DT_MAX` 的脏配置会被跳过。
- `MB_DT_BIT` 的 `raw` 只有 0/1，`scale`/`offset` 依然生效（一般填 1.0 / 0.0）。

### 4.1.1 开关量配置示例（FC01/02 或寄存器状态位）

```
从站：u8fct = MB_FC_READ_COILS(1)，u16CoilsNo = 16（16 个线圈）
       ↓ poll 采集后
       valid_point = ceil(16/16) = 1，reg_data_buf[0] = 打包字（16 个 bit）

点位表：
  {reg_index:0, data_type:MB_DT_BIT, bit_index:0, name:"switch1"}   → 线圈 0
  {reg_index:0, data_type:MB_DT_BIT, bit_index:1, name:"switch2"}   → 线圈 1
  ...
  {reg_index:0, data_type:MB_DT_BIT, bit_index:15, name:"switch16"} → 线圈 15

  线圈 N 的通用公式：reg_index = N/16，bit_index = N%16
  （Modbus 标准：字节内 LSB 在前，与移植库 get_FC1 的拼字顺序一致）

解析结果：{"switch1":1,"switch2":0,...}
```

`MB_DT_BIT` 同样适用于 FC03/04 寄存器里的状态标志位（如告警字 bit3 = 漏水告警）。

### 4.2 下发点位表

```c
mb_point_t points[MB_POINTS_PER_SLAVE];
memset(points, 0, sizeof(points));      // 空位 valid=0

// 温度：reg[0]，uint16，系数 0.1，无偏移
points[0].reg_index = 0;
points[0].data_type = MB_DT_UINT16;
points[0].scale     = 0.1f;
points[0].offset    = 0.0f;
strncpy(points[0].name, "temp", sizeof(points[0].name) - 1);
strncpy(points[0].unit, "°C",  sizeof(points[0].unit) - 1);
points[0].valid = 1;

// 湿度：reg[1]，uint16，系数 0.01，偏移 -40
points[1].reg_index = 1;
points[1].data_type = MB_DT_UINT16;
points[1].scale     = 0.01f;
points[1].offset    = -40.0f;
strncpy(points[1].name, "hum",  sizeof(points[1].name) - 1);
strncpy(points[1].unit, "%RH",  sizeof(points[1].unit) - 1);
points[1].valid = 1;

int ret = mb_pointmap_set_slave_points(1, points);
// MB_STATUS_OK          写入成功
// MB_SLAVE_NOT_FOUND_ERR 该从站未通过 mb_add_slave 注册
// MB_BUILD_REQUEST_ERR   points == NULL
```

要点：

- 必须整表传 32 项，空位 `valid = 0`。
- 从站必须**先** `mb_add_slave` 注册，**再**下发点位表。
- 写入即生效，下一次 `mb_pointmap_parse_slave` 就用新配置，无需重启。
- 写入成功后会触发 `MB_CFG_CHANGE_UPDATE` 回调（`slave_id` = 目标从站）。

### 4.3 读取点位表（Web 回显 / 调试）

```c
mb_point_t out[MB_POINTS_PER_SLAVE];
int n = mb_pointmap_get_slave_points(1, out);
// n >= 0 : 该从站有效点位数量（out 全量 32 项已填）
// n == -1: 从站不存在 / out 为 NULL
```

> 这个接口目前没有内部调用方，属于**给 Web 配置页与调试命令预留的读接口**。

### 4.4 点位表与寄存器地址的关系

`reg_index` 是 `reg_data_buf[]` 的**下标**，与 `telegram.u16RegAdd` 无关：

```
u16RegAdd = 100，u16CoilsNo = 4
  reg_data_buf[0] ↔ 协议地址 100
  reg_data_buf[1] ↔ 协议地址 101
  points[k].reg_index = 1  →  读的是协议地址 101
```

---

## 5. 上报模式：RAW / PARSED / BOTH

```c
typedef enum {
    MB_REPORT_RAW    = 0,   // 只发 {"data":[123,456]}
    MB_REPORT_PARSED = 1,   // 只发 {"temp":25.1,"hum":60}
    MB_REPORT_BOTH   = 2,   // 两种都发
} mb_report_mode_t;
```

- 模式是**全局**的（不是每从站一份），默认 `MB_REPORT_PARSED`。
- 切换：`mb_pointmap_set_report_mode(MB_REPORT_PARSED)`，会触发一次 `MB_CFG_CHANGE_UPDATE`（`slave_id = 0`）。
- 读取：`mb_pointmap_get_report_mode()`。
- 运行期切换立即生效，无需重启。

存储方持久化时，请用 `mb_pointmap_get_report_mode()` 取值一并保存。

---

## 6. MQTT 上报方：pull 接口用法

### 6.1 必须先了解的死锁约束

`mb_for_each_valid_slave` 遍历期间**一直持有 `xSlaveCfgMutex`**（非递归锁）。因此回调里**不能**调用任何内部会再次请求该锁的函数：

```
❌ mb_pointmap_parse_slave()
❌ mb_pointmap_get_slave_points()
❌ mb_take_slave_snapshot()
❌ mb_add_slave() / mb_delete_slave() / mb_realloc_slave_list()
```

回调里**可以**安全做的事：

```
✅ 读 s->telegram.u8id / u8fct / u16RegAdd / u16CoilsNo / enable
✅ 把自己的 ID 记到本地数组
✅ return 非 0 提前结束遍历
```

### 6.2 推荐的两阶段模式

**阶段一**：只收集 ID；**阶段二**：脱离遍历后逐个拉数据、解析、发布。

```c
#define MAX_SLAVES  16

typedef struct {
    uint8_t ids[MAX_SLAVES];
    uint8_t n;
} id_list_t;

static int collect_ids(mb_slave_runtime_t *s, void *ctx)
{
    id_list_t *list = (id_list_t *)ctx;
    if (list->n >= MAX_SLAVES) return 1;          // 非 0 停止遍历
    list->ids[list->n++] = s->telegram.u8id;
    return 0;
}

void vMqttPublishTask(void *argument)
{
    for (;;)
    {
        osDelay(1000);

        id_list_t list = { .n = 0 };
        mb_for_each_valid_slave(collect_ids, &list);   // 这里还在持锁，只收集 ID

        for (uint8_t i = 0; i < list.n; i++)
            publish_one_slave(list.ids[i]);            // 锁已释放，可调任意 pull 接口
    }
}
```

**阶段二**：单个从站的处理，按上报模式分支。

```c
static void publish_one_slave(uint8_t slave_id)
{
    // ---------- RAW / BOTH：原始寄存器 ----------
    mb_slave_snapshot_t snap;
    if (mb_take_slave_snapshot(slave_id, &snap) != MB_STATUS_OK) return;
    if (!snap.data_valid || snap.valid_point == 0) return;   // 无新鲜数据，跳过

    char payload[256];
    int  len = 0;

    mb_report_mode_t mode = mb_pointmap_get_report_mode();

    if (mode == MB_REPORT_RAW || mode == MB_REPORT_BOTH)
    {
        len += snprintf(payload + len, sizeof(payload) - len,
                        "{\"id\":%u,\"data\":[", snap.slave_id);
        for (int j = 0; j < snap.valid_point; j++)
            len += snprintf(payload + len, sizeof(payload) - len,
                            "%s%u", (j ? "," : ""), snap.data[j]);
        len += snprintf(payload + len, sizeof(payload) - len, "]}");
        mqtt_publish_slave(slave_id, payload, len);
    }

    // ---------- PARSED / BOTH：换算后物理量 ----------
    if (mode == MB_REPORT_PARSED || mode == MB_REPORT_BOTH)
    {
        mb_parsed_point_t out[MB_POINTS_PER_SLAVE];
        int n = mb_pointmap_parse_slave(slave_id, out);
        if (n <= 0) return;                                   // 该从站没配点位

        len = 0;
        len += snprintf(payload + len, sizeof(payload) - len,
                        "{\"id\":%u", snap.slave_id);
        for (int i = 0; i < n; i++)
        {
            len += snprintf(payload + len, sizeof(payload) - len,
                            ",\"%s\":%.2f", out[i].name, out[i].value);
            // 需要单位时拼 "name_unit" 键，或改发数组带 unit
        }
        len += snprintf(payload + len, sizeof(payload) - len, "}");
        mqtt_publish_slave(slave_id, payload, len);
    }
}
```

输出对比（同一份数据）：

```jsonc
RAW    : {"id":1,"data":[251,6040]}
PARSED : {"id":1,"temp":25.10,"hum":60.40}
BOTH   : 两条都发（或自行合并成 {"id":1,"data":[...],"parsed":{...}}）
```

### 6.3 单从站查询（Web / MQTT 点查指令）

```c
mb_slave_snapshot_t snap;
if (mb_take_slave_snapshot(slave_id, &snap) != MB_STATUS_OK)
{
    reply_error("slave not found or disabled");
    return;
}
// snap 是锁外拷贝，可自由使用
```

### 6.4 关于 data_valid

- `data_valid` 由 poll 任务在 `ERR_OK_QUERY` 时置 1。
- 采集失败**不会**清 0，表示"最后一次成功的数据仍然可用"。
- 需要判断当前是否在线时看 `is_online`；需要判断"有没有新数据"时看 `data_valid`。
- 如需严格"只发本轮新数据"，调用方自行记录上次发布 tick 与 `last_trigger_tick` 比较，或在发布后置 0（需自行加数据锁）。

---

## 7. 写操作：FC5 / FC6 / FC15 / FC16

```c
mb_status_e mb_inject_write(uint8_t slave_id,
                            uint8_t func_code,      // FC5 / FC6 / FC15 / FC16
                            uint16_t reg_addr,      // 起始寄存器/线圈地址（协议地址，直达从站）
                            uint16_t reg_count,     // FC5/6 强制为 1；FC15/16 ≤ 32
                            const uint16_t *write_values,
                            osSemaphoreId_t result_sem,     // NULL = 异步
                            mb_errot_t *result_err_out,     // NULL = 不要结果
                            uint32_t result_timeout_ms);    // 同步等待超时（ms），0=永久等待
```

**写线圈**：`write_values` 传 0/1。FC05 由移植库自动转 0xFF00/0x0000；
FC15 由 `mb_inject_write` 内部把 0/1 数组打包成 bit 字（bit i = 第 i 个线圈）再发出。
`reg_addr` 是**线圈协议地址本身**（如写线圈 35 就填 35），网关不做寄存器/bit 换算。

### 7.1 同步写（等结果 + 防永久阻塞）

传入 `result_sem` 时，`mb_inject_write` **内部会阻塞等待**（入队成功后由 poll 任务 release，函数自己 acquire），返回即代表本次写已完成。

> ⚠️ **务必设置 `result_timeout_ms`**：传 0 表示永久等待（兼容旧行为），但 poll 任务挂了会卡死。建议 2000~5000ms。

```c
mb_errot_t result = 0;
osSemaphoreId_t sem = osSemaphoreNew(1, 0, NULL);   // 调用方创建

mb_status_e ret = mb_inject_write(1, MB_FC_WRITE_REGISTER, 100, 1,
                                  &value, sem, &result, 3000);   /* 3s 超时 */

osSemaphoreDelete(sem);      // 用完立即删除，函数已内部取走信号

if (ret == MB_WAIT_TIMEOUT_ERR)   /* ★ 同步等待超时，poll 任务可能挂了 */
{
    ack_timeout();
    return;
}
if (ret != MB_STATUS_OK)     // 入队阶段就失败（队列满/参数错），result 未填
{
    ack_fail(ret);
    return;
}

if (result == ERR_OK_QUERY) ack_ok();
else                        ack_fail(result);
```

> ⚠️ 调用方**不要**再 `osSemaphoreAcquire` 一次。信号量容量为 1，函数内部取走后计数归零，再取会永久阻塞。

### 7.2 异步写

```c
mb_inject_write(slave_id, MB_FC_WRITE_MULTIPLE_REGISTERS, 100, 2,
                values, NULL, NULL, 0);   /* 不关心结果，timeout 随便填 */

// 写线圈也一样：合上线圈 35，分断线圈 36~39
uint16_t sw[5] = { 1, 0, 0, 0, 0 };
mb_inject_write(slave_id, MB_FC_WRITE_MULTIPLE_COILS, 35, 5,
                sw, NULL, NULL, 0);
```

### 7.3 行为约定

| 项 | 说明 |
|----|------|
| 优先级 | 写请求队列优先于正常读轮询，poll 任务在 IDLE 先查队列 |
| 重试 | **不重试**。写失败即失败，需要重试请调用方自己再投一次 |
| 拷贝 | `write_values` 内容直接拷入消息体，调用后可释放 |
| 队列深度 | `MB_WRITE_QUEUE_DEPTH = 4` |
| 返回值 | `MB_STATUS_OK` / `MB_HANDLE_STATUS_ERR` / `MB_NOT_SUPPORT_FC_ERR` / `MB_BUILD_REQUEST_ERR` / `MB_QUEUE_FULL_ERR` / `MB_WAIT_TIMEOUT_ERR` |

> 🚨 **ModbusQueryInject 清空队列副作用**：底层 `ModbusQueryInject` 会 `xQueueReset` 清空整个读请求队列（见移植库 `modbus.c:782`）。当前队列深度 2 不太会触发丢包，但如果后续业务扩展到多从站、队列更深，建议把写请求走独立队列避免丢读请求。

---

## 8. 从站增删改 + 上电加载

### 8.1 统一配置入口（MQTT / Web 下发推荐）

```c
mb_status_e mb_apply_slave_config(mb_cfg_op_t op,
                                  uint8_t slave_id,
                                  const modbus_t *cfg,
                                  const mb_point_t *points);
```

一个入口覆盖所有配置场景，内部自动分派：

| op | cfg | points | 效果 |
|----|-----|--------|------|
| `MB_CFG_OP_UPSERT` | ✅ | ✅ | 新增节点 + 点位表（一次完成） |
| `MB_CFG_OP_UPSERT` | ✅ | NULL | 只改从站协议配置 |
| `MB_CFG_OP_UPSERT` | NULL | ✅ | 只改点位表（从站必须已存在） |
| `MB_CFG_OP_UPSERT` | NULL | NULL | 返回 `MB_BUILD_REQUEST_ERR` |
| `MB_CFG_OP_DELETE` | 忽略 | 忽略 | 删除从站（点位表随之失效） |

```c
// 场景 1：MQTT 下发完整节点（telegram + 点位表）
mb_apply_slave_config(MB_CFG_OP_UPSERT, 1, &cfg, points);

// 场景 2：只更新点位表
mb_apply_slave_config(MB_CFG_OP_UPSERT, 1, NULL, points);

// 场景 3：删除节点
mb_apply_slave_config(MB_CFG_OP_DELETE, 1, NULL, NULL);
```

**关键行为**：内部调 `mb_add_slave` + `mb_pointmap_set_slave_points` 时会抑制中间回调，**最终只触发一次** `cfg_change_cb`（reason 按实际情况给 ADD 或 UPDATE），存储方只写一次 Flash。

### 8.2 底层独立接口（仍可用）

```c
mb_status_e mb_add_slave(modbus_t *cfg);       // ID 已存在 → 更新；不存在 → 新增
mb_status_e mb_delete_slave(uint8_t slave_id); // 标记 valid=0
mb_slave_runtime_t *mb_get_slave_runtime(uint8_t slave_id); // 找不到返回 NULL
void mb_notify_reload_complete(void);          // 批量加载收尾
```

`modbus_t` 里框架会自行处理的字段：

- `u16reg` 自动指向该从站的 `reg_data_buf`，外部**不要**赋值；
- `u32CurrentTask` 自动置 NULL。

`mb_add_slave` 数量上限按功能码区分：FC03/04 时 `u16CoilsNo ≤ 32`（寄存器数），
FC01/02 时 `u16CoilsNo ≤ 512`（线圈数，32 字 × 16 bit）。超限返回 `MB_BUILD_REQUEST_ERR`。

**功能码白名单**：读支持 **FC01 / FC02 / FC03 / FC04**，写支持 **FC05 / FC06 / FC15 / FC16**。
`u8fct` 传其它值返回 `MB_NOT_SUPPORT_FC_ERR`。

#### 功能码 + 数量换算集中表

| 方向 | FC | 名称 | `u16CoilsNo` 语义 | RAM 占用 | 上限 |
|------|----|------|-------------------|----------|------|
| 读 | 01 | 读线圈 | 线圈个数 N | `ceil(N/16)` × uint16 打包字 | ≤ 512 |
| 读 | 02 | 读离散输入 | 线圈个数 N | `ceil(N/16)` × uint16 打包字 | ≤ 512 |
| 读 | 03 | 读保持寄存器 | 寄存器个数 N | N × uint16 | ≤ 32 |
| 读 | 04 | 读输入寄存器 | 寄存器个数 N | N × uint16 | ≤ 32 |
| 写 | 05 | 写单线圈 | 固定 1 | — | 1 |
| 写 | 06 | 写单寄存器 | 固定 1 | — | 1 |
| 写 | 15 | 写多线圈 | 线圈个数 N | `ceil(N/16)` × uint16 打包字 | ≤ 512 |
| 写 | 16 | 写多寄存器 | 寄存器个数 N | N × uint16 | ≤ 32 |

> **FC01/02 的数量语义差异**：读线圈/离散输入时，移植库 `get_FC1` 把线圈按 bit 打包
> 进 `reg_data_buf`——N 个线圈占 `ceil(N/16)` 个 uint16（poll 任务的 `valid_point`
> 已按此规则计算，且 N%16≠0 时最后一字的高位残留已自动掩码清零）。
> 配点位表时用 `MB_DT_BIT` 类型 + `bit_index` 提取单个开关，见 4.1.1 节。
> RAW 模式下 `data[]` 输出的是打包字，订阅方需自行解 bit。

### 8.3 上电加载流程

上电从本地配置恢复，与 MQTT 下发**共用同一个入口** `mb_apply_slave_config`，不维护两套路径。
配合批量 API 抑制中间回调，N 个从站只触发 **1 次** RELOAD 回调：

```c
void app_init_modbus(void)
{
    Modbus_Init();                          // 建框架（内部已 ModbusStart）

    mb_set_config_change_cb(on_cfg_change, NULL);

    mb_config_batch_begin();                // ① 开始批量：抑制所有 ADD 回调

    uint8_t count = storage_read_slave_count();
    for (uint8_t i = 0; i < count; i++)
    {
        modbus_t   cfg;
        mb_point_t points[MB_POINTS_PER_SLAVE];

        if (storage_read_slave(i, &cfg) != 0) continue;

        storage_read_points(cfg.u8id, points);

        mb_apply_slave_config(MB_CFG_OP_UPSERT, cfg.u8id, &cfg, points);
    }

    mb_pointmap_set_report_mode(storage_read_report_mode());

    mb_config_batch_end();                  // ② 结束批量：只触发 1 次 RELOAD 回调
}
```

**回调行为对比**：

| 方式 | N 个从站上电加载 |
|------|----------------|
| 裸调 `mb_apply_slave_config` | N 次 ADD + 1 次 RELOAD（`mb_notify_reload_complete`） |
| 包裹 `batch_begin/end` | **0 次 ADD + 1 次 RELOAD** |

> 存储方回调里仍建议置脏标志、由独立存储任务延迟合并写 Flash
> （运行期 MQTT 连续下发多个配置时同样受益）：
>
> ```c
> static volatile uint8_t s_cfg_dirty = 0;
>
> static void on_cfg_change(mb_cfg_change_reason_t reason, uint8_t id, void *ctx)
> {
>     s_cfg_dirty = 1;                       // 只置标志，不做 I/O
> }
>
> void vStorageTask(void *arg)               // 500ms 周期合并写
> {
>     for (;;)
>     {
>         osDelay(500);
>         if (s_cfg_dirty)
>         {
>             s_cfg_dirty = 0;
>             mb_pointmap_flash_save();        // 自己实现的整体快照写
>         }
>     }
> }
> ```

> 上电阶段 poll 任务尚未启动（或从站都还没加），此时调用顺序无所谓。**运行期**修改从站请见第 10 节锁规则。

---

## 9. 持久化接口（仅声明，未实现）

`mb_pointmap_flash.h` 只声明两个函数，**模块内没有实现**，由存储方自行提供函数体：

```c
int mb_pointmap_flash_save(void);   // RAM 配置 → 持久化介质
int mb_pointmap_flash_load(void);   // 持久化介质 → RAM
```

调用约定：

| 时机 | 调用 |
|------|------|
| 配置变更回调触发 | `mb_pointmap_flash_save()`（建议置脏标志，由独立存储任务实际执行 I/O） |
| 上电初始化 | `mb_pointmap_flash_load()`，失败则走默认配置 |

### 需要持久化的内容

```
每个从站：
  telegram: u8id / u8fct / u16RegAdd / u16CoilsNo /
            collect_interval_ms / enable / retry_cnt
  points[MB_POINTS_PER_SLAVE]（整表，含 valid 空位）

全局：
  mb_pointmap_get_report_mode()
```

### 不要持久化的运行时字段

```
last_trigger_tick / fail_count / consecutive_fail /
reg_data_buf[] / valid_point / is_online / data_valid /
collect_status / state / waiting_slave / pending_write
```

### 存储实现要求

1. **整体快照写**，不要逐字段增量写；
2. 写完做完整性校验（CRC32 / 双备份 / 版本号均可），并在文件头放 magic + version；
3. 加载时先校验 magic 再校验完整性，失败回退空配置，不能留半截脏数据；
4. 只在任务上下文调用，**禁止在中断里**；
5. 持 `xSlaveCfgMutex` 期间只做 RAM 内拷贝，真正的擦写放到锁外，避免阻塞 poll 任务。

> 如果暂时不做持久化，可以不调用这两个函数——它们未被任何代码引用，不实现也不会链接失败。一旦有调用点，必须提供实现。

---

## 10. 锁规则与死锁清单

| 锁 | 保护对象 | 谁获取 |
|----|---------|--------|
| `xSlaveCfgMutex` | `slave_list` 结构、`valid`、`telegram`、`points[]` | `mb_for_each_valid_slave`、`mb_take_slave_snapshot`（定位段）、`mb_pointmap_set/get/parse_slave` |
| `xSlaveDataMutex` | `reg_data_buf[]`、`valid_point`、`data_valid` | poll 任务写；`mb_take_slave_snapshot`、`mb_pointmap_parse_slave` 读 |

两把锁**从不嵌套**（框架内部按「先取 config 定位 → 释放 → 再取 data 拷贝」的顺序），因此外部只要不破坏该顺序就不会互相卡死。

### 10.1 死锁清单（务必遵守）

| 场景 | 结果 | 正确做法 |
|------|------|---------|
| 在 `mb_for_each_valid_slave` 回调里调 `mb_pointmap_parse_slave` / `mb_take_slave_snapshot` / `mb_pointmap_get_slave_points` | **自死锁**（同一把 cfg 锁被重复请求） | 回调只收集 ID，退出后再调（第 6.2 节） |
| 在 `mb_for_each_valid_slave` 回调里调 `mb_add_slave` / `mb_delete_slave` | 自死锁 + 破坏遍历 | 遍历结束再改配置 |
| 在配置变更回调里再调用 `mb_add_slave` / `mb_delete_slave` | 递归修改，未定义行为 | 回调里只置脏标志，由别的任务执行修改 |
| 在回调里做 Flash 擦写 / 长耗时 I/O | 阻塞 poll 任务与所有读数据的调用方 | 置标志，交给独立存储任务 |

### 10.2 增删从站的线程约束

`mb_add_slave` / `mb_delete_slave` / `mb_realloc_slave_list` **内部已内置 cfg 锁**（加锁 → 调 `_nolock` 内部版 → 解锁 → 锁外触发回调）。

因此：

- **初始化阶段**（poll 任务未启动）：随便调；
- **运行期**：API 已**内置 cfg 锁**，外部直接调用 `mb_add_slave` / `mb_delete_slave` / `mb_realloc_slave_list` 即可，无需自己加锁；
- 但在 `mb_for_each_valid_slave` 回调里**仍然不能调**这些函数（回调期间持锁，自死锁），见第 10.1 节死锁清单；
- 配置变更回调在**锁外**触发，回调里不要反过来再调 CRUD 函数。

推荐做法：运行期的配置变更由一个专用的「配置任务」串行处理，避免多处并发写配置。

---

## 11. 错误码

```c
typedef enum {
    MB_STATUS_OK = 0,        // 成功
    MB_INIT_STATUS_ERR,      // 初始化失败（malloc/mutex 失败等）
    MB_REALLOC_STATUS_ERR,   // 扩容失败
    MB_HANDLE_STATUS_ERR,    // mb_handle 未初始化 / NULL
    MB_SLAVE_NOT_FOUND_ERR,  // 从站不存在或未启用
    MB_PARSE_STATUS_ERR,     // 解析失败
    MB_BUILD_REQUEST_ERR,    // 参数非法（NULL / 功能码非法 / 数量超限）
    MB_SEND_REQUEST_ERR,     // 发送失败
    MB_QUEUE_FULL_ERR,       // 写请求队列满
    MB_NOT_SUPPORT_FC_ERR,   // 功能码不支持（读只接受 FC01~FC04，写接受 FC5/6/15/16）
    MB_WAIT_TIMEOUT_ERR,     // ★ mb_inject_write 同步等待超时（poll 任务可能挂了）
} mb_status_e;
```

点位接口（`mb_pointmap_*`）返回 `int`，取值同样是上面这套 `mb_status_e`，`get_slave_points` 特殊：返回有效点位数量，`-1` 表示失败。

采集结果用移植库的 `mb_errot_t`，成功判断：

```c
if (result == ERR_OK_QUERY) { /* 成功 */ }
```

---

## 12. 日志等级

在 `mb_api.h` 里改默认值（量产建议 `MB_LOG_LEVEL_OFF`，宏展开为 `((void)0)`，零开销）：

```c
#ifndef MB_LOG_LEVEL
    #define MB_LOG_LEVEL  MB_LOG_LEVEL_INFO
#endif
```

```c
MB_LOGE(fmt, ...)   // 错误
MB_LOGW(fmt, ...)   // 警告
MB_LOGI(fmt, ...)   // 一般信息
MB_LOGD(fmt, ...)   // 调试
```

输出函数集中在 `MB_LOG_OUT` 一处，改成 `rt_kprintf` 或自定义串口输出只需要改这一行。

---

## 13. FAQ

**Q1：`mb_pointmap_get_slave_points` 为什么在仓库里搜不到调用？**
它是给 Web 配置页回显、调试命令查看当前配置用的**预留读接口**，模块内部不需要它。下发走 `set_`，持久化由存储方直接读 `slave_list`。

**Q2：点位表下发后要重启才生效吗？**
不用。`mb_pointmap_set_slave_points` 写完 RAM 即生效，下一次 `mb_pointmap_parse_slave` 就用新表。

**Q3：`reg_index` 填 Modbus 地址还是数组下标？**
数组下标（相对本次读取的起始寄存器）。协议地址 = `telegram.u16RegAdd + reg_index`。

**Q4：多寄存器类型的高低字顺序？**
严格大端：`(reg[i] << 16) | reg[i+1]`。若设备实际是低字在前（word-swap），当前版本不支持——需要在该从站侧先自行交换，或扩展一个 `word_order` 字段。

**Q5：float32 为什么用 memcpy？**
规避严格别名（strict aliasing）未定义行为，编译器优化下强转指针可能读到错误值。

**Q6：32 个点位不够怎么办？**
`MB_POINTS_PER_SLAVE` 与 `MB_REG_MAX_PER_SLAVE` 都硬编码 32。要扩容需同时改：
`mb_pointmap.h` 的 `MB_POINTS_PER_SLAVE`、`mb_api.h` 的 `MB_REG_MAX_PER_SLAVE` 与 `reg_data_buf[]` 尺寸、`mb_write_request_t.write_values[]` 尺寸。注意 RAM 已用约 86%，扩容前先评估。

**Q7：上报模式能不能每个从站单独设？**
不能，当前是全局一份。若需要按从站区分，需把 `s_report_mode` 移进 `mb_slave_runtime_t` 并同步持久化。

**Q8：`data_valid` 和 `is_online` 有什么区别？**
`is_online` = 最近一次通信是否成功；`data_valid` = 是否拿到过有效数据（成功后置 1，失败不清零）。发布前建议两者都看：`is_online` 决定上报"离线"状态，`data_valid` 决定有没有可发的数据。

**Q9：为什么 `mb_for_each_valid_slave` 里不能直接解析？**
它遍历期间持有配置锁（非递归），而 `mb_pointmap_parse_slave` 需要同一把锁做定位，会自死锁。用第 6.2 节的两阶段模式。

**Q10：写请求会不会挤掉读轮询？**
写请求优先：poll 任务每次回到 IDLE 先取写队列。连续大量写请求会推迟读采集，队列深度上限 4。

**Q11：`mb_pointmap_flash_*` 没实现，调用会怎样？**
链接阶段报 `undefined reference`。这两个函数当前**没有调用点**，所以现在不影响构建；存储方接入时补实现即可，也可直接删掉这两个声明。

**Q12：开关量（线圈/离散输入/状态位）怎么配置？**
读侧支持 FC01/02（线圈打包字），写侧支持 FC05/15（`write_values` 传 0/1，内部打包/转换）。
点位表用 `MB_DT_BIT` 类型：`reg_index = 线圈偏移/16`，`bit_index = 线圈偏移%16`，
解析输出 0/1。示例见 4.1.1 节。RAW 模式下 `data[]` 是打包字，订阅方需自行解 bit。

---

## 变更日志

| 版本 | 日期 | 变更内容 | 影响文件 |
|------|------|----------|----------|
| v2.1 | 2026-11-05 | **Bug1 修复**：`mb_handle_init` mutex 创建失败时正确回收已创建的资源 | `mb_slave.c` |
| v2.1 | 2026-11-05 | **Bug2 修复**：CRUD 函数全部内置 cfg 锁（拆成加锁公开版 + `_nolock` 内部版），组合函数持一把锁调用 `_nolock` 避免嵌套死锁；回调统一在**锁外**触发 | `mb_slave.c` / `mb_api.c` / `mb_pointmap.c` / `mb_internal.h` |
| v2.1 | 2026-11-05 | **Bug2 附带修复**：`mb_realloc_slave_list` 扩容后 memset 新区域（之前 realloc 不会清新增区域） | `mb_slave.c` |
| v2.1 | 2026-11-05 | **Bug3 修复**：`mb_inject_write` 增加 `result_timeout_ms` 参数，传 0 兼容旧行为（永久等待），超时返回新错误码 `MB_WAIT_TIMEOUT_ERR` | `mb_api.h` / `mb_api.c` |
| v2.1 | 2026-11-05 | **优化2**：新增 `MB_CFG_CHANGE_GLOBAL` 枚举值，区分"从站变更"与"全局配置变更"；`mb_config_batch_end`、`mb_notify_reload_complete`、`mb_pointmap_set_report_mode` 统一使用 GLOBAL 语义 | `mb_api.h` / `mb_slave.c` / `mb_pointmap.c` |
| v2.1 | 2026-11-05 | **文档补充**：功能码集中表、CRUD 加锁说明更新、`ModbusQueryInject` 清空队列副作用标红、同步等待加超时示例 | 本文件 |
