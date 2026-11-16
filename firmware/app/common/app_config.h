#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 应用全局配置结构体。
 * Config维护基础参数和写权限，M3持久化显式JSON，不保存结构体内存。
 * 最多4站×16点；点表由Config分块读取并直接加载到Modbus运行表。
 * 保存后重启生效；无有效配置时不使用默认平台地址启动业务。
 */

#define CONFIG_MAGIC      0x4D334346U   // "M3CF"
#define CONFIG_VERSION    2U  /* v2: mq_broker_ip[16]→mq_broker_host[64] 支持域名（连云） */
#define CONFIG_MAX_SLAVE  16U           // 保留现有RAM布局，实际准入上限为4
#define CONFIG_ACTIVE_SLAVE_MAX 4U
#define CONFIG_WRITE_MAX 16U

typedef struct
{
    uint8_t slave;
    uint8_t function;
    uint16_t start_address;
    uint16_t end_address;
    uint16_t min_value;
    uint16_t max_value;
} AppControlRule;

#pragma pack(push, 1)
typedef struct
{
    /* ===== 公共头（M3 管理，不要改顺序） ===== */
    uint32_t magic;        // 魔数，标识有效配置
    uint32_t version;      // 配置版本号
    uint32_t crc32;        // RAM兼容保留字段；持久CRC由M3配置槽头维护

    /* ===== Modbus 参数（M1 使用） ===== */
    uint8_t  mb_slave_count;                  // 从站数量（当前默认 4）
    uint8_t  mb_slave_addr[CONFIG_MAX_SLAVE]; // 从站地址列表
    uint32_t mb_baudrate;                     // 串口波特率（默认 115200）
    uint8_t  mb_data_bits;                    // 数据位（默认 8）
    uint8_t  mb_stop_bits;                    // 停止位（默认 1）
    uint8_t  mb_parity;                       // 校验位：0=NONE, 1=ODD, 2=EVEN
    uint8_t  mb_flow_ctrl;                    // 流控制：0=NONE
    uint32_t mb_poll_interval_ms;             // 采集周期（ms）

    /* ===== MQTT 参数（M2 使用） ===== */
    char     mq_broker_host[64];    // Broker 地址（IP 或域名，连云用域名）
    uint16_t mq_broker_port;        // Broker 端口（默认 1883）
    char     mq_client_id[32];      // 客户端 ID
    char     mq_username[32];       // 用户名
    char     mq_password[32];       // 密码
    char     mq_pub_topic[64];      // 上报主题
    char     mq_sub_topic[64];      // 订阅主题
    uint32_t mq_report_interval_ms; // 上报周期（ms）

    uint8_t write_allowlist_count; // 0表示拒绝全部控制写。
    AppControlRule write_allowlist[CONFIG_WRITE_MAX];
} app_config_t;
#pragma pack(pop)

/* ===== 对外读写接口 ===== */
/* 填入出厂默认配置（magic/version/字段默认值） */
void AppConfig_GetDefault(app_config_t *cfg);
/* 当前运行快照只在启动时发布一次；未配置返回NULL，指针在本次启动期间有效。 */
const app_config_t *AppConfig_Get(void);
/* 仅PA0操作所在任务投递TF配置导入；true仅表示受理，完成结果由Config任务记录。 */
bool AppConfig_RequestTfImport(void);

typedef enum
{
    APP_CONFIG_OK,
    APP_CONFIG_INVALID,
    APP_CONFIG_NOT_READY,
    APP_CONFIG_BUSY,
    APP_CONFIG_STORAGE,
    APP_CONFIG_TIMEOUT
} AppConfigResult;

/* 纯字段校验，不访问硬件、修改运行配置或保留cfg指针。 */
AppConfigResult AppConfig_Validate(const app_config_t *cfg);
/* 仅Config_Task在启动读取并校验后发布，试运行也可读取；不写持久数据。 */
bool AppConfig_Activate(const app_config_t *cfg);

/* 控制作业为值对象；不是结构体内存的持久格式或网络格式。 */
#define CONTROL_ID_CAPACITY 65U
#define CONTROL_GATEWAY_CAPACITY 32U
#define CONTROL_VALUES_MAX 32U
#define CONTROL_LIFETIME_MS 300000U
#define CONTROL_FUTURE_SKEW_MS 5000U

typedef enum
{
    CONTROL_ADMISSION,
    CONTROL_PREPARE,
    CONTROL_TRANSMIT
} ControlPhase;

typedef enum
{
    CONTROL_OK,
    CONTROL_FORMAT,
    CONTROL_VERSION,
    CONTROL_RANGE,
    CONTROL_NOT_READY,
    CONTROL_WRONG_TARGET,
    CONTROL_DENIED,
    CONTROL_TIME_INVALID,
    CONTROL_TIME_ROLLBACK,
    CONTROL_EXPIRED,
    CONTROL_POLICY_CHANGED,
    CONTROL_PAUSED,
    CONTROL_NOT_DURABLE
} ControlResult;

typedef struct
{
    uint32_t schema_version;
    char command_id[CONTROL_ID_CAPACITY];
    char gateway_id[CONTROL_GATEWAY_CAPACITY];
    uint8_t slave, function;
    uint16_t address, count;
    uint16_t values[CONTROL_VALUES_MAX];
    uint32_t policy_generation;
    uint64_t issued_at_ms, expires_at_ms;
    uint64_t received_at_ms, received_monotonic_ms, deadline_ms;
    uint64_t event_seq;
} ControlJob;

typedef struct
{
    const app_config_t *config;
    const char *gateway_id;
    uint32_t generation;
    bool paused, storage_ready;
    bool accepted_durable, executing_durable;
} ControlPolicy;

typedef struct
{
    uint64_t utc_ms, monotonic_ms, control_time_floor_ms;
    bool valid;
} ControlTimeSnapshot;

/*
 * 无I/O的统一控制校验，不修改输入或保留指针；可在最终发送短临界区调用。
 * policy须来自本次启动已校验的配置、稳定身份及该作业真实持久状态，不能取网络布尔值。
 * gateway_id须在32 B内终止。ADMISSION使用同一次接收快照固定received_*及deadline；
 * 后续阶段沿用它们，不能重新计时。PREPARE要求ACCEPTED，TRANSMIT另要求EXECUTING。
 * FC05为原始0000/FF00，按0/1比较；FC15按低位在前打包，未用位/字必须为0；
 * FC06/16为原始u16。数量1～32，单写必须1；一条白名单须覆盖完整地址段和全部值。
 * CONTROL_OK仅表示此快照通过；不会写日志、入队或授权直接TX，发送前仍须按真实状态复验。
 */
ControlResult ControlValidate(const ControlJob *job, const ControlPolicy *policy,
                              const ControlTimeSnapshot *time, ControlPhase phase);

#endif /* APP_CONFIG_H */
