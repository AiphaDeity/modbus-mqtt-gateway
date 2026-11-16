# Modbus 接口与配置说明

核对日期：2026-11-15。本文面向配置、采集和上报的调用方，依据当前源码；不把底层可支持的功能码当作产品已开放的入口。

当前容量为 **4 个从站 × 每站 16 点**，每站最多缓存 32 个寄存器。现场通过 TF 的 `/config.json` 配置，保存后手动重启生效；不需要按设备型号重新编译。当前配置读取功能码仅支持 03/04，完整控制持久执行尚未接通，旧直接写接口明确拒绝。

- 实际配置文件和导入步骤：[第15节](#15-tf配置与启动加载)。
- 上报取数与锁约束：[第6节](#6-mqtt上报取数)。
- 升级暂停与恢复边界：[第14节](#14-升级暂停监督与维护帧)。
- 引脚与内存：[硬件与运行资源](../../../../docs/hardware-resources.md)。

## 1. 模块职责与文件

| 文件 | 职责 |
| --- | --- |
| [mb_api.h / mb_api.c](mb_api.h) | 初始化、加载配置、快照、遍历及对外状态码 |
| [mb_poll.c](mb_poll.c) | 从站轮询、在途请求和采集结果处理 |
| [mb_pointmap.h / mb_pointmap.c](mb_pointmap.h) | 点位类型、字节序和数值换算 |
| [mb_runtime.c](mb_runtime.c) | 配置/发送准入、升级暂停结算及监督 |
| [modbus.h / modbus.c](modbus.h) | RTU 主机、帧编解码与串口交互 |
| [config/config.c](../config/config.c) | 完整 JSON 校验、TF 保存和启动加载 |
| [mqtt/telemetry_task.c](../mqtt/telemetry_task.c) | 取快照、解析点位、生成遥测 |

Modbus 独占 RS485，配置和网络任务通过接口交互；Flash 与 FatFs 由 Storage_Task 独占。接口成功、采集成功、持久化成功及云端接收是不同结果。

## 2. 调用入口速查

| 调用方 / 目的 | 入口 | 完成含义 |
| --- | --- | --- |
| 装配层 | `Modbus_Init()`、`mb_runtime_ready()` | 创建模块资源并检查初始化状态，不等于从站在线 |
| Config 启动加载 | `Modbus_LoadConfig(cfg)`、`Modbus_LoadSlave(cfg, points)` | 初始化总线和运行表、逐站加载；全部校验成功后发布配置 |
| 遥测收集从站 | `mb_for_each_valid_slave(cb, ctx)` | 只遍历有效且启用的从站；回调期间持配置锁 |
| 遥测取原始值 | `mb_take_slave_snapshot(id, out)` | 将数据复制到调用方缓冲，成功后仍须判断采集状态 |
| 遥测取物理量 | `mb_pointmap_parse_slave(id, out)` | 输出点位结果，逐项检查 `valid` |
| 查询点表 | `mb_pointmap_get_slave_points(id, out)` | 点表副本；不是配置导出或网络命令 |
| 按键请求配置导入 | `AppConfig_RequestTfImport()` | 仅表示请求被受理，保存结果由 Config 任务报告 |
| 旧直接写入口 | `mb_inject_write(...)` | 固定拒绝，不发总线，见第7节 |

## 3. 数据结构与所有权

`modbus_t` 的必要字段为 `u8id`（从站地址）、`u8fct`（功能码）、`u16RegAdd`（协议起始地址）、`u16CoilsNo`（数量）、`collect_interval_ms`（周期）、`enable` 和 `retry_cnt`。`u16reg` 由模块绑定缓存，外部不得替换。

`mb_slave_snapshot_t` 包含从站地址、功能码、在线状态、`data_valid`、起始地址、有效长度、`data[32]`、采集 tick 和结果码。输出是值拷贝；仅使用 `valid_point` 指定的部分，不访问未填充尾部。`last_collect_tick` 当前来自最近一次采集触发 tick，不是 RTC 时间或最近成功的墙钟时间。

`mb_point_t` 保存 `reg_index`、`data_type`、`scale`、`offset`、`name`、`unit`、`valid`、`bit_index` 和 `byte_order`。C 接口点表缓冲按 `MB_POINTS_PER_SLAVE`（16 项）提供，空位 `valid=0`；`name`、`unit` 数组均为 8 B，需为末尾 NUL 留 1 B。

解析输出为 `mb_parsed_point_t`：名称、单位、`float value`、原类型及 `valid`。浮点换算非有限时该项无效；大整数转换为 float 可能损失精度。外部不得保存遍历回调中的内部运行对象指针供后续异步使用。

## 4. 点位解释规则

协议地址 = 从站配置 `address + reg_index`。例如 `address=100`、`reg_index=2` 表示协议地址 102；`reg_index` 不是设备手册中的完整寄存器地址。

| data_type | 类型 | 寄存器数量 |
| --- | --- | --- |
| 0 | uint16 | 1 |
| 1 | int16 | 1 |
| 2 | uint32 | 2 |
| 3 | int32 | 2 |
| 4 | float32 / IEEE 754 | 2 |

物理值 = 原始解释值 × `scale` + `offset`。点位所需的全部寄存器必须位于本站连续读取区间内。

| byte_order | 16 位 | 32 位 |
| --- | --- | --- |
| 0 | AB | ABCD |
| 1 | BA | BADC |
| 2 | 不接受 | CDAB |
| 3 | 不接受 | DCBA |

这里 A/B/C/D 表示按寄存器顺序取得的四个字节；换序只影响解释，不改变缓存原始值。底层枚举另有 `MB_DT_BIT=5`，但当前 TF JSON 不接受此类型，也不开放 01/02 读取配置；不能据底层代码推断它们已是产品配置能力。

## 5. 上报模式

模块保留全局枚举 `MB_REPORT_RAW=0`、`MB_REPORT_PARSED=1`、`MB_REPORT_BOTH=2`，当前初值为 PARSED。`mb_pointmap_get_report_mode()` 查询模式；设置接口受配置门控，运行配置发布后拒绝热改。

当前 TF JSON 没有上报模式字段，遥测实际格式以 `telemetry_task.c` 为准。枚举存在不代表已经有现场切换或模式持久化功能。

## 6. MQTT上报取数

### 6.1 锁约束

`mb_for_each_valid_slave()` 遍历期间持有非递归配置锁。回调中不得再次取快照、解析/修改点表、增删从站、访问 Flash 或等待网络发布；否则可能重入同一把锁或长时间阻塞轮询。

### 6.2 推荐的两阶段模式

现有 `telemetry_task.c` 已采用此顺序，接入时复用，不另建发布任务：

1. `collect_slave_ids()` 回调只把 ID 复制进最多 4 项的本地列表。
2. 遍历返回、配置锁释放后，逐个 ID 调用 `mb_take_slave_snapshot()`。
3. 检查返回码、有效长度、`data_valid`、`is_online` 和 `collect_status`，区分无数据、旧数据和采集失败。
4. 需要物理量时调用 `mb_pointmap_parse_slave()`，仅使用有效点位；随后在锁外组织和发送消息。

快照和解析是两次独立加锁读取，不承诺属于完全相同的一次采样。返回的原始值与点位结果各自完整，调用方不能把两个调用误认为一个原子快照。

### 6.3 有效性与失败

当前成功采集会置 `data_valid=1`、`is_online=1`；采集失败置 `is_online=0`，不会清掉已有的 `data_valid`。因此 `data_valid=1` 只说明存在可用历史数据，不保证本轮采集成功，也不是可由消费者清零的消息标志。

快照失败时不使用输出；解析数量为零或点位 `valid=0` 时不把默认数值冒充正常采样。MQTT 发布完成也不等于云端业务回执完成。

## 7. 写控制当前边界

旧 `mb_inject_write(...)` 不包含完整命令身份、有效期和持久受理信息，当前固定返回 `MB_CONTROL_REJECTED_ERR`；不读取 `write_values`、不入队、不发送 RS485。若 `result_err_out` 非空，会写入 `ERR_CONTROL_REJECTED`。

`write_allowlist` 已可随 TF 配置校验和保存，但完整控制通知、权限、持久执行与结果恢复尚未接通。不能用旧接口绕过这些检查，也不能把底层 05/06/15/16 编解码支持描述成已可用的远程控制。

## 8. 启动加载与配置生效

1. 装配层初始化模块并创建原有任务，Config 向 Storage 读取本次固定运行槽 R。
2. 完整解析 JSON 并核对 CRC，成功后 `Modbus_LoadConfig()` 初始化总线及空运行表。
3. 再读同一槽，逐站调用 `Modbus_LoadSlave()`，复核完整内容后 `AppConfig_Activate()` 发布运行快照。
4. 轮询只使用该快照配置的地址及点位；无配置时 `AppConfig_Get()` 为 NULL，不自动启用默认从站。

`mb_apply_slave_config()`、点表设置、上报模式设置等修改接口受同一门控；运行快照发布后返回 `MB_UPGRADE_BUSY_ERR`。当前没有 Web 或 MQTT 在线配置入口，现场修改统一走 TF 保存后重启，详见第15节。

## 9. 配置持久化

Config 与 Storage 保存完整、已校验的原始 JSON，复用现有配置三槽；不另存一份 Modbus 点表副本。运行缓存、锁、指针、队列、tick、在线状态和未完成请求不属于配置文件。

旧资料中的 `mb_pointmap_flash_save/load` 不是当前实现入口；不要按旧示例新增它们。配置变更回调仅用于通知，不授权调用任务直接擦写 Flash。

## 10. 并发与缓冲约束

- 普通任务调用数据接口；不得在 ISR 中等待模块互斥锁。
- 使用快照/点表读取接口取副本，不直接读写 `slave_list` 或 `reg_data_buf`。
- 遍历回调仅收集 ID；对外发布、存储及耗时计算放在回调外。
- 点表和总线配置只在 Config 启动加载阶段生效；运行期间不旁路热改。
- 异步接入不得保存调用者栈指针；实际 I/O 必须在所属任务完成，并依据返回值判断结束。

## 11. 对外状态码

以下为 `mb_status_e`，与 RTU 的 `mb_errot_t` 采集结果分开解释。

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 0 | MB_STATUS_OK | 接口成功 |
| 1 | MB_INIT_STATUS_ERR | 初始化失败 |
| 2 | MB_REALLOC_STATUS_ERR | 固定从站槽已满，不执行扩容 |
| 3 | MB_HANDLE_STATUS_ERR | 句柄或必要输出无效 |
| 4 | MB_SLAVE_NOT_FOUND_ERR | 从站不存在或不可用 |
| 5 | MB_PARSE_STATUS_ERR | 解析失败 |
| 6 | MB_BUILD_REQUEST_ERR | 构造请求失败 |
| 7 | MB_SEND_REQUEST_ERR | 启动发送失败 |
| 8 | MB_QUEUE_FULL_ERR | 请求队列满 |
| 9 | MB_NOT_SUPPORT_FC_ERR | 功能码不支持 |
| 10 | MB_WAIT_TIMEOUT_ERR | 等待结果超时 |
| 11 | MB_UPGRADE_BUSY_ERR | 暂停、试运行、配置已发布或其他准入条件不满足 |
| 12 | MB_CONTROL_REJECTED_ERR | 旧直接写入口被禁止 |

## 12. 日志

日志等级由 `mb_api.h` 的 `MB_LOG_LEVEL` 控制：OFF=0、ERROR=1、WARN=2、INFO=3、DEBUG=4，默认 INFO。采集日志是诊断信息，不能替代返回码、状态及云端业务确认。

## 13. 常见配置问题

- 手册写“40001”时，先确认其是否为显示编号；JSON `address` 使用实际 Modbus 协议地址，不直接照抄显示前缀。
- 32 位值需要连续两个寄存器；字节序按实际设备协议填写，不能仅凭数值看似合理判断正确。
- 每站当前只配置一个连续读取区间；不连续地址需在 32 寄存器范围内包含所需区间，不能任意添加独立轮询块。
- 修改 TF 配置并保存成功后必须手动重启，当前运行快照不会即时变化。
- 从站离线不等于网关故障；应区别通信超时、旧数据与本地任务停止推进。

## 14. 升级暂停监督与维护帧

`Modbus_Init()` 绑定 MODBUS 暂停角色。Pause 关闭新轮询/写准入，Poll 处理未发送请求，Master 结束已发送事务；Settle 核对队列、Poll/Master 在途状态、UART TX、配置调用计数及未消费结果。Sync 只确认总线和结果均结算，不访问 Flash；失败保持暂停，不通过清空队列伪造完成。

真正启动 UART 发送前再次检查暂停、试运行/全局门控及请求资格。试运行可做只读采样，控制写禁止；已发送写请求结果不明为 `ERR_UNKNOWN`，不得自动重发。

Poll 根据真实状态推进报告进展，同时检查 Master；监督进展与单次等待当前上限均为 3000 ms。合法从站超时可降级，本地执行链停止推进或 UART 启动失败会锁存故障；这个软件期限不等于硬件 IWDG 的复位时间。

共用 [maint_frame.h](../../../shared/maint_frame.h) 只负责编解码，不拥有 UART 或命令会话。维护帧 V1 为 A5 5A、version、opcode、flags、request_id(u32LE)、payload_len(u16LE≤1024)、payload、CRC32LE，总开销 15 B；CRC 覆盖 version 至 payload。成功解码的 payload 引用输入缓冲，必须先消费再移动缓冲；MORE 保留半帧，错误按 consumed 丢弃前缀重同步。

Boot 的 USART2 恢复已接通并有实板记录，具体命令、半帧/会话期限和接线见[升级说明](../upgrade/README.md#boot恢复维护帧)。App 不开放串口配置命令。完整 Modbus 负载及与升级并发的验证仍需独立实板证据。

## 15. TF配置与启动加载

PA0稳定长按至少3 s后松开，Storage只检查TF根目录的两个固定文件：

| 根目录文件 | 行为 |
| --- | --- |
| 只有/config.json | 原Config任务受理配置，完整校验后保存；手动重启才生效 |
| 只有/firmware.gwp | 现有升级任务执行安全包导入 |
| 两者都有或都没有 | 拒绝本次操作，不擦候选、不删除文件 |

其他文件和/records目录可照常保留，不参与选择。不会因上电或插卡自动导入，操作后也不自动删除配置或升级包。前述操作仍需正常启动身份和业务准入；试运行或升级暂停期间不接受配置写入。

固定字段格式保留version=2，以下示例只用于说明字段；地址、类型、字节序和连接信息应按实际配置填写，不预设设备型号：

~~~json
{
  "version": 2,
  "mb_slave_count": 1,
  "mb_slave_addr": [
    1
  ],
  "mb_baudrate": 115200,
  "mb_data_bits": 8,
  "mb_stop_bits": 1,
  "mb_parity": 0,
  "mb_flow_ctrl": 0,
  "mb_poll_interval_ms": 1000,
  "mq_broker_host": "mqtt.example.com",
  "mq_broker_port": 1883,
  "mq_client_id": "gw_01",
  "mq_username": "",
  "mq_password": "",
  "mq_pub_topic": "gw_01/data",
  "mq_sub_topic": "gw_01/cmd",
  "mq_report_interval_ms": 5000,
  "mb_devices": [
    {
      "slave": 1,
      "function": 3,
      "address": 0,
      "count": 2,
      "period_ms": 1000,
      "enabled": 1,
      "retries": 0,
      "points": [
        {
          "reg_index": 0,
          "data_type": 4,
          "byte_order": 0,
          "scale": 1,
          "offset": 0,
          "name": "value",
          "unit": ""
        }
      ]
    }
  ],
  "write_allowlist": []
}
~~~

- mb_slave_count/mb_slave_addr/mb_devices顺序及数量须一致，最多4站；slave为1～247且不重复。
- 每站function仅3/4，address+count不超过65536，count为1～32；period_ms为100～3600000，retries为0～3，enabled为0/1。启用站至少配置一个点位。
- 每站最多16点，reg_index相对本次读取区间；data_type为0:uint16、1:int16、2:uint32、3:int32、4:float32，32位占两个寄存器。byte_order含义见第4节。
- scale/offset支持有限十进制和指数，均须明确；name为最多7 B的ASCII字母、数字、下划线或连字符，同站不重复。unit为最多7 B UTF-8，不含控制字符、引号或反斜杠；字符串支持引号/反斜杠/斜杠转义，不接受Unicode转义缩写，可直接使用UTF-8字符。
- write_allowlist最多16项，每项slave/function/start_address/end_address/min_value/max_value均须明确；只接受5/6/15/16，线圈原始值0～1、寄存器0～65535，整段地址和最小/最大值有序。当前已完成字段保存及校验；控制通知身份、时效和持久执行接入尚未完成，不能以配置导入成功宣称控制闭环已经完成。
- 整份JSON最多64 KiB，按256 B分块解析，拒绝未知、重复、缺失字段、尾随内容和非法UTF-8。完整JSON点表与原最小17字段配置不同，不自动迁移旧开发配置。

第一次完整校验不修改运行点表；启动再次读取同一固定R后逐站加载，最后发布运行快照。TF导入先完整校验，再从稳定文件租约第二遍保存原文，两遍CRC不同拒绝；保存沿用三槽、固定15 s期限和原暂停结算。文件关闭失败或提交不明时保留pending并阻止下一次操作，不通过清空状态假装结束。

AppConfig_RequestTfImport返回true只表示按键请求已受理，Config任务输出保存结果及是否需要重启。配置仅从TF文件导入，保存原文，不提供无调用方的内存候选提交、JSON重新编码或点表导出接口。文件或候选租约未结算时继续阻止新操作；复用一个栈内存储消息和一站点表暂存。
