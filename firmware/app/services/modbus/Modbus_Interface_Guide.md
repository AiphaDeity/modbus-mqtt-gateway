# Modbus 模块对外接口文档

> 版本：v1.0  
> 适用对象：**MQTT 数据上报方**、**配置/存储方**  
> 头文件：`#include "modbus_app.h"`

---

## 目录

- [架构总览](#架构总览)
- [MQTT 数据上报方](#一-mqtt-数据上报方)
  - [接口清单](#mqtt-方接口清单)
  - [核心结构体](#mqtt-方核心结构体)
  - [场景 A：定时扫描所有从站上报](#场景-a定时扫描所有从站上报推荐)
  - [场景 B：查询单个从站数据](#场景-b查询单个从站数据)
  - [场景 C：下发写操作（FC6 / FC16）](#场景-c下发写操作fc6--fc16)
- [配置存储方](#二-配置存储方)
  - [接口清单](#存储方接口清单)
  - [核心结构体与回调类型](#存储方核心结构体与回调类型)
  - [场景 A：上电初始化（Flash → Modbus）](#场景-a上电初始化flash--modbus)
  - [场景 B：运行时配置变更自动写 Flash](#场景-b运行时配置变更自动写-flash)
  - [需要持久化的字段清单](#需要持久化的字段清单)
- [公共接口（双方都可能用到）](#三公共接口双方都可能用到)
- [互斥锁使用规则](#四互斥锁使用规则)
- [日志等级控制](#五日志等级控制)
- [常见问题 FAQ](#六常见问题-faq)

---

## 架构总览

```
                           ┌────────────────────────────┐
                           │      modbus_app 模块       │
                           │                            │
   外部模块（MQTT/存储） ───│── mb_inject_write()        │───▶ 写寄存器（FC6/FC16）
                           │── mb_for_each_valid_slave()│───▶ 遍历所有有效从站
                           │── mb_take_slave_snapshot() │───▶ 安全读单个从站快照
                           │── mb_set_config_change_cb()│───▶ 注册配置变更回调
                           │── mb_add_slave()           │───▶ 增/改从站
                           │── mb_delete_slave()        │───▶ 删从站
                           │── mb_handle_get()          │───▶ 拿全局句柄
                           │                            │
                           │  内部实现：                 │
                           │  - FreeRTOS 状态机轮询任务  │
                           │  - 单槽一问一答串口驱动    │
                           │  - 阶梯冷却自动恢复        │
                           └────────────────────────────┘
```

---

## 一、MQTT 数据上报方

### MQTT 方接口清单

| 函数 | 作用 | 什么时候用 |
|------|------|------------|
| `mb_for_each_valid_slave(cb, ctx)` | 遍历所有 valid && enable 的从站 | ⭐ **99% 定时上报用这个** |
| `mb_take_slave_snapshot(id, &snap)` | 按 ID 安全拷贝一份数据快照 | Web 查单设备、MQTT 点查指令 |
| `mb_inject_write(...)` | 下发写操作（FC6/FC16） | MQTT 下发写寄存器指令 |
| `mb_handle_get()` | 拿全局句柄 | 需要直接访问底层字段时 |

### MQTT 方核心结构体

```c
// 遍历回调（mb_for_each_valid_slave 的第二个参数）
typedef int (*mb_slave_iter_cb_t)(mb_slave_runtime_t *slave, void *ctx);
// 返回值：0 = 继续遍历；非 0 = 立即停止

// 数据快照（mb_take_slave_snapshot 的输出）
typedef struct {
    uint8_t  slave_id;           // 从站地址
    uint8_t  is_online;          // 在线状态（1/0）
    uint16_t reg_addr;           // 寄存器起始地址
    uint16_t valid_point;        // 有效数据长度
    uint16_t data[32];           // 数据快照（已拷贝，可自由读）
} mb_slave_snapshot_t;

// 运行时结构体（遍历回调里拿到的 slave 指针）
struct mb_slave_runtime {
    uint8_t        valid;            // 1=有效；0=空位
    modbus_t       telegram;         // 协议配置（见下方字段）
    uint32_t       last_trigger_tick;
    uint8_t        fail_count;
    uint8_t        consecutive_fail; // 阶梯冷却计数
    uint16_t       reg_data_buf[32]; // ⭐ 寄存器数据缓存
    uint16_t       valid_point;      // ⭐ 当前有效数据长度
    uint8_t        is_online;        // ⭐ 在线状态
    mb_errot_t     collect_status;
    uint8_t        data_valid;       // ⭐ 1=有有效数据可发；0=跳过
};

// telegram（modbus_t）里你可能关心的字段
typedef struct {
    uint8_t  u8id;              // 从站地址
    uint8_t  u8fct;             // 功能码（3=读保持寄存器，4=读输入寄存器）
    uint16_t u16RegAdd;         // 寄存器起始地址
    uint16_t u16CoilsNo;        // 读取数量（≤32）
    uint16_t *u16reg;           // 指向 reg_data_buf（内部自动设置，外部不要动）
    uint32_t collect_interval_ms; // 采集周期
    uint8_t  enable;            // 是否启用
    uint8_t  retry_cnt;         // 本轮重试次数
    // ... 其他移植库字段略
} modbus_t;
```

---

### 场景 A：定时扫描所有从站上报（推荐）

最常用的模式。MQTT 任务每 500ms 扫一次，遍历回调里拿锁读数据、组 payload、发出去。

```c
#include "modbus_app.h"
#include "mqtt_client.h"

/* ============= MQTT 定时发布任务 ============= */
void vMqttPublishTask(void *argument)
{
    for (;;)
    {
        osDelay(500);  // 每 500ms 扫一次

        // 一行代码：遍历所有 valid==1 && enable==1 的从站
        // 内部自动加 xSlaveCfgMutex 配置锁、自动过滤
        mb_for_each_valid_slave(on_publish_one_slave, NULL);
    }
}

/* ============= 遍历回调：处理单个从站 ============= */
// 注意：回调在 xSlaveCfgMutex 持有中执行
//       不要在回调里再调 mb_add_slave / mb_delete_slave / mb_take_slave_snapshot
//       （会再拿同一把锁 → 死锁）
static int on_publish_one_slave(mb_slave_runtime_t *s, void *ctx)
{
    // 1. 跳过未初始化或无有效数据的从站
    if (!s->data_valid || s->valid_point == 0) return 0;

    // 2. 拿数据锁（配置锁 + 数据锁，两把一起用）
    //    配置锁防止 valid 被改，数据锁防止 reg_data_buf 被 poll 半写
    mb_handle_t *mh = mb_handle_get();
    osMutexAcquire(mh->xSlaveDataMutex, osWaitForever);

    // 3. 组 payload
    char topic[32];
    snprintf(topic, sizeof(topic),
             "modbus/slave/%d/data", s->telegram.u8id);  // topic 里带从站 ID

    char payload[256];
    int len = snprintf(payload, sizeof(payload),
        "{\"online\":%d,\"addr\":%u,\"data\":[",
        s->is_online, s->telegram.u16RegAdd);

    for (int j = 0; j < s->valid_point; j++)
    {
        len += snprintf(payload + len, sizeof(payload) - len,
                        "%s%u", (j > 0) ? "," : "", s->reg_data_buf[j]);
    }
    len += snprintf(payload + len, sizeof(payload) - len, "]}");

    osMutexRelease(mh->xSlaveDataMutex);

    // 4. 发布 MQTT
    MQTTPublish(topic, payload, len);

    return 0;  // 0 = 继续遍历下一个从站；非 0 立即停止
}
```

---

### 场景 B：查询单个从站数据

Web 页面点某个设备、MQTT 收到查询指令时使用。

```c
/* 查询单个从站 */
void mqtt_on_query_slave(uint8_t slave_id)
{
    mb_slave_snapshot_t snap;
    mb_status_e ret = mb_take_slave_snapshot(slave_id, &snap);

    if (ret != MB_STATUS_OK)
    {
        // 从站不存在 / 未启用
        mqtt_reply_error(slave_id, "not found or disabled");
        return;
    }

    // snap 里的数据已经是加锁拷贝出来的，直接用
    char payload[256];
    int len = snprintf(payload, sizeof(payload),
        "{\"slave\":%d,\"online\":%d,\"addr\":%u,\"valid\":%u,\"data\":[",
        snap.slave_id, snap.is_online, snap.reg_addr, snap.valid_point);

    for (int j = 0; j < snap.valid_point; j++)
    {
        len += snprintf(payload + len, sizeof(payload) - len,
                        "%s%u", (j > 0) ? "," : "", snap.data[j]);
    }
    len += snprintf(payload + len, sizeof(payload) - len, "]}");

    mqtt_reply(slave_id, payload, len);
}
```

---

### 场景 C：下发写操作（FC6 / FC16）

MQTT 收到写寄存器指令时调用。

#### FC6：同步写单个寄存器（等结果）

```c
void mqtt_on_write_single(uint8_t slave_id, uint16_t reg_addr, uint16_t value)
{
    mb_errot_t write_result;
    osSemaphoreId_t done_sem = osSemaphoreNew(1, 0, NULL);

    mb_status_e ret = mb_inject_write(
        slave_id,                    // 目标从站地址
        MB_FC_WRITE_REGISTER,        // 功能码 0x06
        reg_addr,                    // 寄存器地址
        1,                           // FC6 固定传 1
        &value,                      // 写入值（函数内部会拷贝，不依赖外部指针生命周期）
        done_sem,                    // 同步信号量（写完成后 poll 任务 release）
        &write_result                // 结果输出指针（poll 任务处理完后填值）
    );

    if (ret != MB_STATUS_OK)
    {
        // 请求没投入队列（队列满或参数错）
        osSemaphoreDelete(done_sem);
        mqtt_ack_fail(slave_id, "queue full or invalid param");
        return;
    }

    // 阻塞等待 Modbus 写完成
    osSemaphoreAcquire(done_sem, osWaitForever);
    osSemaphoreDelete(done_sem);

    if (write_result == ERR_OK_QUERY)
        mqtt_ack_success(slave_id);
    else
        mqtt_ack_fail(slave_id, write_result);
}
```

#### FC16：异步写多个寄存器（不等结果）

```c
void mqtt_on_write_multi(uint8_t slave_id, uint16_t reg_addr,
                         uint16_t count, const uint16_t *values)
{
    mb_status_e ret = mb_inject_write(
        slave_id,
        MB_FC_WRITE_MULTIPLE_REGISTERS,  // 功能码 0x10
        reg_addr,
        count,                            // 写入数量（≤ 32）
        values,                           // 函数内部会拷贝，传 const 安全
        NULL,                             // NULL = 不等结果
        NULL                              // NULL = 不关心结果
    );

    if (ret == MB_STATUS_OK)
        MQTT_LOG("write queued: slave=%d, addr=%u, count=%u",
                 slave_id, reg_addr, count);
}
```

#### 参数说明

```c
mb_status_e mb_inject_write(
    uint8_t         slave_id,       // 1~247
    uint8_t         func_code,      // MB_FC_WRITE_REGISTER(6) 或 MB_FC_WRITE_MULTIPLE_REGISTERS(16)
    uint16_t        reg_addr,       // 起始寄存器地址
    uint16_t        reg_count,      // FC6 固定传 1；FC16 传写入数量（≤ 32）
    const uint16_t *write_values,   // 值数组（函数内部 memcpy，调用后可释放）
    osSemaphoreId_t result_sem,     // 同步等待信号量（传 NULL 表示异步）
    mb_errot_t     *result_err_out  // 结果输出指针（传 NULL 忽略）
);
// 返回值:
//   MB_STATUS_OK              成功投入队列
//   MB_HANDLE_STATUS_ERR      mb_handle 还没初始化
//   MB_NOT_SUPPORT_FC_ERR     功能码不是 6 或 16
//   MB_BUILD_REQUEST_ERR      参数非法（values 为 NULL 或 count > 32）
//   MB_QUEUE_FULL_ERR         队列满（深度 4），丢请求
```

---

## 二、配置存储方

### 存储方接口清单

| 函数 | 作用 | 什么时候用 |
|------|------|------------|
| `Modbus_Init()` | 初始化 Modbus 框架 | 上电第一步 |
| `mb_set_config_change_cb(cb, ctx)` | 注册配置变更回调 | `Modbus_Init` 之后、加载 Flash 之前 |
| `mb_add_slave(&cfg)` | 新增 / 更新从站 | 上电从 Flash 加载时；MQTT 改配置时 |
| `mb_delete_slave(id)` | 删除从站 | MQTT 下发删除指令时 |
| `mb_notify_reload_complete()` | 通知加载完成 | 上电逐个 add 完最后调一次 |
| `mb_handle_get()` | 拿全局句柄 | 回调里遍历从站列表写 Flash 时 |

### 存储方核心结构体与回调类型

```c
// 配置变更原因（回调第一个参数）
typedef enum {
    MB_CFG_CHANGE_ADD     = 0,   // mb_add_slave 成功新增
    MB_CFG_CHANGE_UPDATE  = 1,   // mb_add_slave 更新已有（ID 重复）
    MB_CFG_CHANGE_DELETE  = 2,   // mb_delete_slave 成功
    MB_CFG_CHANGE_RELOAD  = 3,   // 上电批量加载完（mb_notify_reload_complete 触发）
} mb_cfg_change_reason_t;

// 配置变更回调函数类型
typedef void (*mb_cfg_change_cb_t)(
    mb_cfg_change_reason_t reason,   // 变更原因（见上表）
    uint8_t                slave_id,  // 涉及的从站 ID（RELOAD 时为 0）
    void                  *user_ctx  // 注册时传入的 ctx（当前传 NULL 即可）
);

// telegram 里需要持久化的字段（详见下方清单）
modbus_t {
    u8id, u8fct, u16RegAdd, u16CoilsNo,
    collect_interval_ms, enable, retry_cnt, ...
};
```

---

### 场景 A：上电初始化（Flash → Modbus）

```c
#include "modbus_app.h"
#include "flash_driver.h"

/* ============= 存储方实现的回调（Modbus 模块不 include Flash 头文件，零耦合） ============= */
static void on_mb_config_change(mb_cfg_change_reason_t reason,
                                 uint8_t slave_id, void *ctx)
{
    mb_handle_t *mh = mb_handle_get();

    // 用配置锁保护从站列表结构
    osMutexAcquire(mh->xSlaveCfgMutex, osWaitForever);

    // 整体快照模式：不管哪个从站变了，遍历整个列表写 Flash
    for (uint8_t i = 0; i < mh->slave_max_num; i++)
    {
        mb_slave_runtime_t *s = &mh->slave_list[i];
        if (!s->valid) continue;

        // 写 telegram 里的配置字段（见下方持久化清单）
        flash_write_slave(i, &s->telegram);
    }
    flash_commit();

    osMutexRelease(mh->xSlaveCfgMutex);

    MB_LOGI("Flash saved, reason=%d, slave=%d", reason, slave_id);
}

/* ============= 上电初始化流程 ============= */
void app_init_modbus_storage(void)
{
    // 1. 先建 Modbus 框架
    Modbus_Init();

    // 2. 注册回调（任何增删改自动触发）
    mb_set_config_change_cb(on_mb_config_change, NULL);

    // 3. 从 Flash 逐个加载从站配置
    uint8_t count = flash_read_slave_count();
    for (int i = 0; i < count; i++)
    {
        modbus_t cfg;
        if (flash_read_slave(i, &cfg) == FLASH_OK)
        {
            mb_add_slave(&cfg);  // 自动触发 MB_CFG_CHANGE_ADD → 回调写 Flash
        }
    }

    // 4. 通知加载完成（触发 MB_CFG_CHANGE_RELOAD）
    mb_notify_reload_complete();

    // 5. 正式开始 Modbus 通信（如果 Modbus_Init 里已经 Start 了就跳过）
    // ModbusStart();
}
```

---

### 场景 B：运行时配置变更自动写 Flash

MQTT 下发配置 → 存储方调用 `mb_add_slave` / `mb_delete_slave` → Modbus 内部自动触发回调 → 回调自动写 Flash。**完全不用手动处理 Flash 写入时机。**

```c
/* MQTT 下发新增或更新从站配置 */
void mqtt_on_set_slave_config(modbus_t *new_cfg)
{
    // ID 已存在 → MB_CFG_CHANGE_UPDATE → 自动触发回调写 Flash
    // ID 不存在 → MB_CFG_CHANGE_ADD    → 自动触发回调写 Flash
    mb_add_slave(new_cfg);
}

/* MQTT 下发删除从站 */
void mqtt_on_delete_slave(uint8_t slave_id)
{
    // → MB_CFG_CHANGE_DELETE → 自动触发回调写 Flash
    mb_delete_slave(slave_id);
}
```

---

### 需要持久化的字段清单

```
telegram (modbus_t) 里要存的配置字段：
├── u8id                 从站地址 (1~247)
├── u8fct                功能码 (3=读保持寄存器, 4=读输入寄存器)
├── u16RegAdd            寄存器起始地址
├── u16CoilsNo           读取数量 (≤ 32)
├── collect_interval_ms  采集周期 (ms)
├── enable               是否启用 (1/0)
├── retry_cnt            本轮重试次数

不要存的运行时字段（上电自动清零）：
├── consecutive_fail     阶梯冷却计数
├── fail_count            本轮重试计数
├── last_trigger_tick    上次采集 tick
├── reg_data_buf[32]     数据缓存
├── valid_point           有效数据长度
├── is_online             在线状态
├── data_valid            数据有效标志
├── collect_status        上次采集状态
└── state / waiting_slave / pending_write 等状态机字段
```

---

## 三、公共接口（双方都可能用到）

### mb_handle_get() — 拿全局句柄

```c
mb_handle_t *mh = mb_handle_get();
// mh->slave_list              从站数组
// mh->slave_max_num           数组容量（最多 16）
// mh->slave_actual_num        当前有效从站数
// mh->xSlaveCfgMutex          配置锁（保护 valid/enable 等字段）
// mh->xSlaveDataMutex         数据锁（保护 reg_data_buf）
// mh->state / waiting_slave   轮询状态（外部一般不要碰）
```

### mb_get_slave_runtime() — 按 ID 查从站

```c
mb_slave_runtime_t *s = mb_get_slave_runtime(slave_id);
// 找不到返回 NULL
```

### mb_add_slave() / mb_delete_slave() — 底层增删

```c
// 新增或更新（ID 已存在则更新，不存在则新增）
mb_status_e mb_add_slave(modbus_t *cfg);
// 返回：
//   MB_STATUS_OK              成功
//   MB_HANDLE_STATUS_ERR      mb_handle 未初始化
//   MB_BUILD_REQUEST_ERR      u16CoilsNo > 32 被拒绝

// 删除
mb_status_e mb_delete_slave(uint8_t slave_id);
// 返回：
//   MB_STATUS_OK              成功
//   MB_SLAVE_NOT_FOUND_ERR    找不到这个从站
```

---

## 四、互斥锁使用规则

```
┌──────────────────┬───────────────────────────────┬──────────────────────┐
│ 锁                │ 保护范围                       │ 持有中的限制          │
├──────────────────┼───────────────────────────────┼──────────────────────┤
│ xSlaveCfgMutex    │ valid / enable                 │ ❌ 不要调             │
│  (配置锁)          │ 从站列表增删改                  │ mb_add_slave /       │
│                   │                               │ mb_delete_slave      │
├──────────────────┼───────────────────────────────┼──────────────────────┤
│ xSlaveDataMutex   │ reg_data_buf[]                 │ 没特别限制，一把锁    │
│  (数据锁)          │ valid_point                    │ 只保护数组读写        │
│                   │ data_valid                    │                      │
└──────────────────┴───────────────────────────────┴──────────────────────┘
```

### 正确的加锁模式（推荐）

```c
// 模式1：遍历回调里
static int on_publish_one_slave(mb_slave_runtime_t *s, void *ctx)
{
    // 配置锁已由 mb_for_each_valid_slave 持有，valid 不会变
    // 只需要额外拿数据锁读 reg_data_buf
    mb_handle_t *mh = mb_handle_get();
    osMutexAcquire(mh->xSlaveDataMutex, osWaitForever);
    // 读 s->reg_data_buf / s->valid_point / s->is_online
    osMutexRelease(mh->xSlaveDataMutex);
    return 0;
}

// 模式2：不用遍历，自己按 ID 查
void some_func(void)
{
    mb_handle_t *mh = mb_handle_get();
    mb_slave_runtime_t *s = mb_get_slave_runtime(3);
    if (s)
    {
        osMutexAcquire(mh->xSlaveDataMutex, osWaitForever);
        // 读数据
        osMutexRelease(mh->xSlaveDataMutex);
    }
}

// 模式3：用快照函数（内部已正确加锁）
mb_slave_snapshot_t snap;
mb_take_slave_snapshot(3, &snap);
// snap 里的数据已经安全，直接用
```

---

## 五、日志等级控制

在 `modbus_app.h` 开头通过宏控制。**量产前设为 `MB_LOG_LEVEL_OFF` 关掉所有日志**。

```c
// modbus_app.h 中修改默认值
#ifndef MB_LOG_LEVEL
    #define MB_LOG_LEVEL  MB_LOG_LEVEL_INFO   // 调试用
    // #define MB_LOG_LEVEL  MB_LOG_LEVEL_OFF  // 量产后用这个
#endif

// 可选等级：
#define MB_LOG_LEVEL_OFF    0   // 关全部
#define MB_LOG_LEVEL_ERROR  1   // 只打错误
#define MB_LOG_LEVEL_WARN   2   // 错误 + 警告
#define MB_LOG_LEVEL_INFO   3   // 一般信息（默认）
#define MB_LOG_LEVEL_DEBUG  4   // 全部调试
```

调用示例：
```c
MB_LOGE("write FAIL: slave=%d, err=%d", id, err);
MB_LOGW("queue full, slave=%d", id);
MB_LOGI("read ok: slave=%d, reg0=%d", id, val);
MB_LOGD("tick=%u, state=%d", now, state);
```

宏展开后是空 `((void)0)`，编译器完全优化掉——零开销。

---

## 六、常见问题 FAQ

### Q: MQTT 方不知道有多少从站，怎么遍历？
**A:** 直接调 `mb_for_each_valid_slave(on_publish_one, NULL)`，内部帮你扫完所有 valid 从站。你不需要知道数量或 ID。

### Q: `mb_inject_write` 写失败会自动重试吗？
**A:** 不会。写操作不重试——避免设备状态不一致。如果需要重试，调用方自己再投一次。

### Q: 遍历回调里能调 `mb_take_slave_snapshot` 吗？
**A:** 不能。`mb_for_each_valid_slave` 内部持有配置锁，`mb_take_slave_snapshot` 会再拿同一把锁 → **死锁**。遍历回调里直接加数据锁读 `slave->reg_data_buf` 就行。

### Q: 从站被 delete 后，MQTT 下次扫到会怎么样？
**A:** `mb_for_each_valid_slave` 自动跳过 valid==0 的从站，不会触发回调。MQTT 方不会收到这个从站的数据，topic 可以主动清理或等 MQTT 遗嘱超时。

### Q: Flash 回调里要不要拿锁？
**A:** `mb_for_each_valid_slave` 内部已经拿了配置锁，回调在锁持有中执行。但配置变更回调（`on_mb_config_change`）是在 `mb_add_slave` / `mb_delete_slave` 内部触发的，**那里没有持锁**——回调里要自己拿 `xSlaveCfgMutex`。上面示例代码里已经加了。

### Q: 上电加载从站后，为什么还要调 `mb_notify_reload_complete()`？
**A:** 因为逐个 `mb_add_slave` 时每个都会触发一次回调（`MB_CFG_CHANGE_ADD`），Flash 会被写 N 次。`mb_notify_reload_complete` 再触发一次 `MB_CFG_CHANGE_RELOAD`，存储方可以在 RELOAD 回调里做收尾（比如整体 CRC 校验、写完成标记）。如果你的存储方用的是整体快照模式（每次回调都遍历整个列表重写），那 RELOAD 回调可能已经不需要了——但调用它不会有副作用。

### Q: 写请求投到队列后，Modbus poll 正在处理读请求怎么办？
**A:** 写请求投入的是**独立的写请求队列**，poll 任务每次从读队列出来后，在 IDLE 状态**优先检查写请求队列**。也就是说：写请求 > 读轮询，紧急写操作会抢占正常的读采集周期。

### Q: 队列满了（深度 4）怎么办？
**A:** `mb_inject_write` 返回 `MB_QUEUE_FULL_ERR`，日志打一条 `[MB-W] write req queue full`。调用方可以选择：
- 异步模式：忽略（MQTT 下发配置一般不会同时 4 个以上写）
- 同步模式：`osDelay(100)` 后重试

### Q: reg_data_buf 是 32 个寄存器，外部可以存更多吗？
**A:** 不能。硬编码 `MB_REG_MAX_PER_SLAVE = 32`。`mb_add_slave` 会拒绝 `u16CoilsNo > 32` 的配置，防止移植库 `get_FC3` 写爆数组。