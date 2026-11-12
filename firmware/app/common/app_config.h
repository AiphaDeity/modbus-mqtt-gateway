#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 应用全局配置结构体。
 * 由 M3 持久化到 W25Q128 SPI Flash。
 * M1（Modbus）和 M2（MQTT）的参数都在这里定义。
 *
 * 注意：
 *   - 用 #pragma pack(1) 保证结构体大小固定，跨版本一致。
 *   - 新增字段只能加在 reserved 之前，且必须递增 CONFIG_VERSION。
 *   - 总大小不要超过 256 字节（W25Q128 一个页足够）。
 */

#define CONFIG_MAGIC      0x4D334346U   // "M3CF"
#define CONFIG_VERSION    2U  /* v2: mq_broker_ip[16]→mq_broker_host[64] 支持域名（连云） */
#define CONFIG_MAX_SLAVE  16U           // 最多支持的从站数量

#pragma pack(push, 1)
typedef struct
{
    /* ===== 公共头（M3 管理，不要改顺序） ===== */
    uint32_t magic;        // 魔数，标识有效配置
    uint32_t version;      // 配置版本号
    uint32_t crc32;        // CRC32 校验（覆盖 magic 到 reserved 之前的所有字段）

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

    /* ===== 预留（后续扩展用） ===== */
    uint8_t  reserved[64];
} app_config_t;
#pragma pack(pop)

/* ===== 对外读写接口（app_config.c 实现） ===== */
/* 填入出厂默认配置（magic/version/字段默认值） */
void AppConfig_GetDefault(app_config_t *cfg);
/* 获取当前生效配置指针（只读）；未加载时返回默认配置 */
const app_config_t *AppConfig_Get(void);
/* 仅任务上下文更新RAM；试运行/升级暂停返回false，不表示已持久保存。 */
bool AppConfig_Set(const app_config_t *cfg);

#endif /* APP_CONFIG_H */
