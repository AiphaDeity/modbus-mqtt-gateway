# M2（MQTT 上云模块）→ M3（存储模块）接口约定（草案 v0.1）

> 目的：按会议纪要《M2 需要给到 M3 的信息》逐项给出 M2 侧的定稿/建议，M3 据此实现 `Storage_Init` / `Storage_Task`。
> 依据：`tests/mqtt/*.json`（云端报文正反例，已定稿）、`docs/hardware-resources.md`（任务/队列分工）。
> 标注：✅已定（不用再讨论）　🟡建议（M2 提议，M3 可提反对意见）　🔴待拍板（需要会上定）

---

## 0. 一句话

M2 是"记录的生产者"：现场数据（telemetry）和事件（event）在**离线时**先让 M3 存进 TF 卡，联网后 M2 再找 M3 把历史记录**补传**上去、传完回执让 M3 删除。云端报文格式已经定稿（`tests/mqtt/`），M3 **不需要理解业务字段**，只要按本文第 3 节的"记录壳"存取即可。

---

## 1. M2 需要持久化的配置字段（`app_config_t`）

> 说明：配置区物理上在 W25Q128（M3 的 SPI Flash），读写服务归 M1（Config 模块）。M2 只**定义字段清单和默认值**，M3/M1 按这个结构体存盘。
> 建议落在 Flash 的配置区独立 4 KB 扇区，**不要和 TF 卡记录区混**。

```c
/* app_config_t —— 存进 SPI Flash 的 M2 配置块（建议总大小 ≤ 256 字节，尾部 CRC16） */
#define APP_CONFIG_MAGIC   0x32434D32u   /* '2CM2' */
#define APP_CONFIG_VERSION 1u

typedef struct {
  uint32_t magic;            /* 块有效性校验 */
  uint16_t version;          /* = APP_CONFIG_VERSION */

  /* --- MQTT 连接 --- */
  char     mqtt_host[64];    /* 服务器 IP 或域名，如 "192.168.2.10" / "broker.example.com" */
  uint16_t mqtt_port;        /* 默认 1883（8883 预留 TLS，当前不支持） */
  char     client_id[32];    /* 设备 clientID，默认 = gateway_id */
  uint16_t keepalive_s;      /* 心跳周期，默认 60 s */
  uint8_t  clean_session;    /* 默认 1 */

  /* --- 身份与 topic（前缀建议拼成 "{gateway_id}/xxx"，M2 内部拼，配置里只存 topic 模板前缀） --- */
  char     gateway_id[24];   /* 本机唯一 ID，如 "mb2mqtt_001"——record_id 前缀来源，✅已定 */
  char     topic_sub[48];    /* 下行命令订阅 topic，默认 "{gateway_id}/cmd" */
  char     topic_telemetry[48]; /* 采集上行发布 topic，默认 "{gateway_id}/telemetry" */
  char     topic_event[48];  /* 事件上行发布 topic，默认 "{gateway_id}/event" */
  char     topic_status[48]; /* 状态上报 topic，默认 "{gateway_id}/status" */
} app_config_t;
/* 尾部另放 uint16_t crc16（覆盖上述全部字段）；
   magic/CRC 任一不对 → M1 使用编译期默认值并标记 time_valid=false。 */
```

🟡 待 M1 确认：配置块是否需要预留 32 字节缓冲区给后续字段（建议是）。

---

## 2. 记录 ID 规则（✅已定，来自 `tests/mqtt/telemetry_*`）

云端报文里的 `record_id` 格式：

```
record_id = "{gateway_id}:{kind}:{seq_decimal_string}"

例： mb2mqtt_001:seq:1001   （telemetry 采集记录）
     mb2mqtt_001:evt:0001   （event 事件记录）
```

- `kind`：`seq`=telemetry，`evt`=event。
- `seq`：**uint64 单调递增序号**，从 1001 开始，**跨重启连续**（存在 TF 卡记录区，不依赖 RTC）；JSON 里按**十进制字符串**传输（正反例已明确：数字类型会被平台 REJECT）。
- M3 侧内部存储**只用 (kind, seq) 定位**，不必存 record_id 字符串——补传时 M2 按记录内容自己拼字符串。
- `boot_id`：每次启动生成（如 `boot_%u`，写 RTC/RAM，不持久化）；`uptime_ms` 辅助。

---

## 3. TF 卡单条记录二进制格式（🟡 M2 提议，本节是 M3 实现存储的核心契约）

### 3.1 设计原则

- **payload 就是该条记录的 JSON 原文（UTF-8）**，即与 `tests/mqtt/telemetry_valid.json` 里 `records[]` 数组中的**单个元素**序列化后的 JSON 文本。
  → 补传时 M2 把 M3 给的记录**原样重发**即可，零业务转换；M3 永远不需要理解里面是什么字段。
- 存**单条**而不是整批（batch）：因为 ACK 是按 record_id 精确确认的，单条存储才能精确删除。
- 定长记录头 + 变长 payload；小端。

### 3.2 字节排布（小端，STM32 native，免转换）

```
偏移   长度   字段            说明
────────────────────────────────────────────────────────────
 0     2     magic          固定 0x5245（'ER'），扫卡/坏块恢复用
 2     8     captured_at_ms Unix 时间戳(ms)；未校时 = 0
10     1     kind           1=telemetry，2=event
11     8     seq            记录序号 uint64，单调递增
19     2     payload_len    payload 字节数（≤ 512）
21     N     payload        JSON 原文 UTF-8
21+N   2     crc16          从偏移 0 到 payload 末尾的 CRC16（CRC-16/CCITT-FALSE）
────────────────────────────────────────────────────────────
记录总长 = 23 + payload_len + 2
```

| 项 | 约定 | 理由 |
|---|---|---|
| 字节序 | **小端（little-endian）** ✅ | STM32 native，M3/M2 同芯片，零转换；TF 卡只在本机读 |
| 定长头 | 23 字节 | 便于 M3 按定长步长扫盘定位坏记录 |
| payload 上限 | ≤ 512 字节 🟡 | 单条 JSON 实际约 300 字节；超出由 M2 截断上报（status.alarms） |
| 校验 | 尾部 CRC16 | 扫盘发现坏记录直接跳过，不阻塞补传队列 |

> 🟡 如果 M3 倾向 FatFs 落盘（文件系统而不是裸扇区）：上述"记录壳"不变，只是存储介质从"裸扇区环形区"变成"一个追加写的日志文件 + 索引文件（kind,seq → 文件偏移）"。**接口字段不变，M2 无感。**

### 3.3 TF 卡满策略（🟡 M2 提议）

**循环覆盖最旧记录（环形）**，而不是停写丢新数据：

- 理由：现场设备无人维护，断网可能数天；停写会丢**最新**数据（最关键）。环形策略保证"最近的数据永远在"。
- 覆盖时 M3 维护 `dropped` 计数，M2 在 status 报文 `cache_stats.dropped` 上报（字段已在 `tests/mqtt/status_valid.json` 定稿）。
- 🔴 待拍板：环形区容量（建议按 32 KB 扇区对齐，先给 **2000 条 ≈ 1 MB** 起，实测后调）。
- M2 提供 `pending` 查询：M3 随时报告当前未补传条数，M2 填 `cache_stats.pending`。

---

## 4. M2 ↔ M3 队列协议（🟡 M2 提议，FreeRTOS 消息队列，内存紧张请给上限）

> 双方任务优先级都 = Normal(24)，用队列解耦；**所有消息指针只在本块生命周期内有效**，M3/M2 收到消息后内部拷贝，不持有对方指针跨队列。

### 4.1 M2 → M3：写入一条待持久化记录（离线时）

```c
typedef struct {
  uint8_t  kind;           /* 1=telemetry, 2=event */
  uint64_t seq;
  uint64_t captured_at_ms;
  const uint8_t *payload;  /* JSON UTF-8；M3 拷贝进自己的缓冲，M2 不持有 */
  uint16_t payload_len;
} storage_write_req_t;
/* 队列深度建议 8；写失败（卡满/卡故障）由 M3 内部处理，
   M2 从 pending 查询接口感知，不阻塞采集任务。 */
```

### 4.2 M2 → M3：补传拉取请求（联网后）

```c
typedef struct {
  uint32_t req_id;      /* M2 生成，匹配请求/响应 */
  uint16_t max_count;   /* 一次最多取几条，建议 ≤ 8（配合 M2 栈 4KB） */
} storage_fetch_req_t;
```

### 4.3 M3 → M2：补传数据应答

```c
typedef struct {
  uint32_t req_id;
  uint16_t count;                 /* 实际条数（0 = 没有历史记录了） */
  struct {
    uint8_t  kind;
    uint64_t seq;
    uint64_t captured_at_ms;
    uint16_t payload_len;
    uint8_t  payload[512];       /* 最大单条定长缓冲；队列消息整体 ≤ ~560 字节 */
  } records[8];
} storage_fetch_resp_t;
```

### 4.4 M2 → M3：上传成功回执（CacheAck，让 M3 删除）

```c
typedef struct {
  uint64_t ack_upto_seq;   /* kind 维度：kind+seq ≤ 此值的连续段已上传成功，可删 */
} storage_ack_t;
```

> 因为 `seq` 单调递增，回执只需要一个水位线，不用传 record_id 列表（云端 ack 报文里的 `record_ids: ["gw:seq:1001", ...]` 是给平台看的，内部不重复一份）。

### 4.5 M2 ↔ M3：状态查询（可选，M3 主动报或 M2 轮询）

```c
/* M2 填 status 报文 cache_stats 需要 */
typedef struct {
  uint16_t pending;   /* 当前未补传条数 */
  uint16_t dropped;   /* 因环形覆盖丢失的条数 */
} storage_cache_stats_t;
```

---

## 5. M2 这边已定、不用 M3 关心的事

- 云端 5 类报文 JSON 结构全部定稿，样例见 `tests/mqtt/`：
  - `telemetry_valid.json`（采集上报）、`events_valid.json`（事件）、`status_valid.json`（在线状态）、`acks_valid.json`（上传回执）、`commands_valid.json`（平台下行命令）。
  - 平台侧拒绝规则也在 `telemetry_invalid.json` 里写明了（缺字段、quality 枚举小写、seq 写成数字、record_id 前缀不符、时间自相矛盾、未知 schema_version）——**M3 不用管，但 M2 拼报文时会守这些规则**。
- 队列消息**不在中断里发**；所有存储 I/O 在 `Storage_Task` 内做，SDIO DMA 由 M3 串行占用（见 hardware-resources：SDIO RX/TX 复用 DMA2 Stream3，方向切换由 M3 管）。
- M2 不直接碰 SDIO/SPI Flash/RTC 任何寄存器。

---

## 6. 需要和 M3/M1 会上拍板的 4 件事 🔴

1. **环形记录区容量**：建议 2000 条（≈1 MB），是否够用？
2. **存储介质形态**：裸扇区环形区 vs FatFs 日志文件——M3 选哪个都行，M2 接口不变。
3. **队列消息深度**：写队列/取队列各 8 够不够（RAM 已用 86.67%，不能开大数组）。
4. **`app_config_t` 落盘扇区分配**：SPI Flash 上配置区和参数区怎么划（M1 主责，M2 只给字段）。

---

### 版本

| 日期 | 内容 |
|---|---|
| 2026-11-04 | v0.1 草案：按会议纪要逐项给出，标注已定/建议/待拍板 |
