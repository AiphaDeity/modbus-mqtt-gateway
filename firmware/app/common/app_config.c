/*
 * app_config.c —— 应用全局配置实现
 *
 * 功能：
 *   1. 提供显式使用的字段默认值，不自动启用业务
 *   2. 提供本次启动固定的运行快照
 *
 * 设计：
 *   - 默认值集中在这里，改参数只改这一处
 *   - 配置存在 SPI Flash（W25Q128）里，掉电不丢
 *   - Config通过Storage_Task加载；无配置时不自动启用默认值
 */
#include "app_config.h"
#include "app_modules.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "stm32f4xx.h"
#include <string.h>

/* 仅供调用方初始化候选，空槽或损坏配置不自动使用。 */
static const app_config_t s_default_config = {
    /* 公共头 */
    .magic = CONFIG_MAGIC,
    .version = CONFIG_VERSION,
    .crc32 = 0,  /* CRC 由 Storage 模块算 */

    /* Modbus 参数（M1） */
    .mb_slave_count = 4,
    .mb_slave_addr = {1, 2, 3, 4},
    .mb_baudrate = 115200,
    .mb_data_bits = 8,
    .mb_stop_bits = 1,
    .mb_parity = 0,        /* NONE */
    .mb_flow_ctrl = 0,     /* NONE */
    .mb_poll_interval_ms = 1000,

    /* MQTT 参数（M2） */
    .mq_broker_host = "192.168.2.45",   /* 本地 mosquitto 默认（阿里云模式走宏不走这里） */
    .mq_broker_port = 1883,
    .mq_client_id = "mb2mqtt_001",
    .mq_username = "",
    .mq_password = "",
    .mq_pub_topic = "mb2mqtt_001/telemetry",
    .mq_sub_topic = "mb2mqtt_001/cmd",
    .mq_report_interval_ms = 5000,
};

/* 启动加载校验后发布一次，保存候选不修改此快照。 */
static app_config_t s_config;
static bool s_loaded;

/**
 * @brief  填入候选默认值，不激活配置或写入存储
 * @param  cfg 输出参数，拷贝默认配置到这里
 */
void AppConfig_GetDefault(app_config_t *cfg)
{
    memcpy(cfg, &s_default_config, sizeof(app_config_t));
}

/**
 * @brief  获取当前配置（全局实例）
 * @return 配置指针
 */
const app_config_t *AppConfig_Get(void)
{
    return s_loaded ? &s_config : NULL;
}

/**
 * @brief 启动时发布已校验的运行快照；运行期间不允许热切换
 */
bool AppConfig_Activate(const app_config_t *cfg)
{
    if (__get_IPSR() != 0U || !App_IsCurrentModuleTask(APP_MODULE_CONFIG) ||
        AppConfig_Validate(cfg) != APP_CONFIG_OK)
        return false;
    taskENTER_CRITICAL();
    bool allowed = !s_loaded;
    if (allowed)
    {
        memcpy(&s_config, cfg, sizeof(app_config_t));
        s_loaded = true;
    }
    taskEXIT_CRITICAL();
    return allowed;
}

// 校验有界标识，只允许ASCII字母、数字、下划线和连字符，拒绝未终止输入。
static bool control_id_valid(const char *id, uint32_t capacity)
{
    if (id == NULL || id[0] == '\0') return false;
    for (uint32_t i = 0U; i < capacity; i++)
    {
        char value = id[i];
        if (value == '\0') return true;
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '_' || value == '-')) return false;
    }
    return false;
}

// 复用相同参数、权限及时效规则；阶段升级只增加真实持久状态要求，不放宽规则。
ControlResult ControlValidate(const ControlJob *job, const ControlPolicy *policy,
                              const ControlTimeSnapshot *time, ControlPhase phase)
{
    if (job == NULL || policy == NULL || time == NULL ||
        (uint32_t)phase > CONTROL_TRANSMIT ||
        !control_id_valid(job->command_id, sizeof(job->command_id)) ||
        !control_id_valid(job->gateway_id, sizeof(job->gateway_id))) return CONTROL_FORMAT;
    if (job->schema_version != 1U) return CONTROL_VERSION;
    bool coil = job->function == 5U || job->function == 15U;
    bool single = job->function == 5U || job->function == 6U;
    if ((!coil && job->function != 6U && job->function != 16U) ||
        job->slave == 0U || job->slave > 247U || job->count == 0U ||
        job->count > CONTROL_VALUES_MAX || (single && job->count != 1U) ||
        (uint32_t)job->address + job->count > 65536U) return CONTROL_RANGE;
    if (job->function == 5U && job->values[0] != 0U && job->values[0] != 0xFF00U)
        return CONTROL_RANGE;
    uint32_t words = job->function == 15U ? (job->count + 15U) / 16U : job->count;
    if (job->function == 15U && job->count % 16U != 0U &&
        (job->values[words - 1U] >> (job->count % 16U)) != 0U) return CONTROL_RANGE;
    for (uint32_t i = words; i < CONTROL_VALUES_MAX; i++)
        if (job->values[i] != 0U) return CONTROL_RANGE;

    const app_config_t *cfg = policy->config;
    if (cfg == NULL || cfg->magic != CONFIG_MAGIC || cfg->version != CONFIG_VERSION ||
        cfg->mb_slave_count > CONFIG_ACTIVE_SLAVE_MAX || cfg->write_allowlist_count > CONFIG_WRITE_MAX ||
        policy->generation == 0U || !control_id_valid(policy->gateway_id, CONTROL_GATEWAY_CAPACITY) ||
        !policy->storage_ready) return CONTROL_NOT_READY;
    if (strcmp(job->gateway_id, policy->gateway_id) != 0) return CONTROL_WRONG_TARGET;
    if (job->policy_generation != policy->generation) return CONTROL_POLICY_CHANGED;
    if (policy->paused) return CONTROL_PAUSED;
    if (phase != CONTROL_ADMISSION && (!policy->accepted_durable || job->event_seq == 0U))
        return CONTROL_NOT_DURABLE;
    if (phase == CONTROL_TRANSMIT && !policy->executing_durable) return CONTROL_NOT_DURABLE;

    if (!time->valid || time->utc_ms == 0U || time->monotonic_ms < job->received_monotonic_ms)
        return CONTROL_TIME_INVALID;
    if (time->utc_ms < time->control_time_floor_ms) return CONTROL_TIME_ROLLBACK;
    if (job->expires_at_ms <= job->issued_at_ms ||
        job->expires_at_ms - job->issued_at_ms > CONTROL_LIFETIME_MS ||
        job->received_at_ms == 0U || job->expires_at_ms <= job->received_at_ms ||
        (job->issued_at_ms > job->received_at_ms &&
         job->issued_at_ms - job->received_at_ms > CONTROL_FUTURE_SKEW_MS) ||
        (job->issued_at_ms > time->utc_ms &&
         job->issued_at_ms - time->utc_ms > CONTROL_FUTURE_SKEW_MS)) return CONTROL_RANGE;
    uint64_t remaining = job->expires_at_ms - job->received_at_ms;
    if (remaining > CONTROL_LIFETIME_MS + CONTROL_FUTURE_SKEW_MS ||
        remaining > UINT64_MAX - job->received_monotonic_ms ||
        job->deadline_ms != job->received_monotonic_ms + remaining ||
        (phase == CONTROL_ADMISSION && (job->received_at_ms != time->utc_ms ||
                                       job->received_monotonic_ms != time->monotonic_ms)))
        return CONTROL_RANGE;
    if (time->utc_ms >= job->expires_at_ms || time->monotonic_ms >= job->deadline_ms)
        return CONTROL_EXPIRED;

    bool configured = false;
    for (uint32_t i = 0U; i < cfg->mb_slave_count; i++)
        if (cfg->mb_slave_addr[i] == job->slave) configured = true;
    if (!configured) return CONTROL_DENIED;
    for (uint32_t i = 0U; i < cfg->write_allowlist_count; i++)
    {
        const AppControlRule *rule = &cfg->write_allowlist[i];
        if (rule->slave != job->slave || rule->function != job->function ||
            rule->start_address > job->address ||
            rule->end_address < (uint32_t)job->address + job->count - 1U ||
            rule->start_address > rule->end_address || rule->min_value > rule->max_value ||
            (coil && rule->max_value > 1U)) continue;
        bool allowed = true;
        for (uint32_t j = 0U; j < job->count; j++)
        {
            uint16_t value = job->function == 5U ? (job->values[0] != 0U ? 1U : 0U) :
                             job->function == 15U ? ((job->values[j / 16U] >> (j % 16U)) & 1U) :
                             job->values[j];
            if (value < rule->min_value || value > rule->max_value) allowed = false;
        }
        if (allowed) return CONTROL_OK;
    }
    return CONTROL_DENIED;
}
