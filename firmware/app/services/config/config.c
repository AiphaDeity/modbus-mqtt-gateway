/*
 * config.c — 配置的启动加载、TF导入、候选保存和升级暂停。
 * 复用三槽存储，分块校验固定字段和点表；保存后重启生效。
 */
#include "app_config.h"
#include "app_modules.h"
#include "app_readiness.h"
#include "app_monitor.h"
#include "app_upgrade.h"
#include "upgrade_quiesce.h"
#include "storage_runtime.h"
#include "storage_upgrade_tf.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "stm32f4xx_hal.h"
#include "mb_api.h"
#include "mb_pointmap.h"            /* mb_point_t / mb_data_type_t 点位表结构 */
#include "modbus.h"                 /* modbus_t 从站协议结构体 */
#include "mini_json.h"              /* 精简JSON解析器（配置变更命令解析用） */
#include "config_task.h"            /* Config_HandleMessage 对外声明 */

#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define CONFIG_JSON_MAX (64U * 1024U)
#define CONFIG_IO_MS 15000U
#define CONFIG_POLL_MS 50U

typedef enum { CFG_NUMBER, CFG_TEXT, CFG_FLOAT, CFG_ADDRESSES, CFG_DEVICES, CFG_POINTS, CFG_ALLOW } cfg_field_type_t;
typedef struct
{
    const char *name;
    uint16_t offset;
    uint16_t size;
    cfg_field_type_t type;
} cfg_field_t;

#define CFG_FIELD(member, kind) {#member, offsetof(app_config_t, member), sizeof(((app_config_t *)0)->member), kind}
static const cfg_field_t s_fields[] =
{
    CFG_FIELD(version, CFG_NUMBER),
    CFG_FIELD(mb_slave_count, CFG_NUMBER),
    CFG_FIELD(mb_slave_addr, CFG_ADDRESSES),
    CFG_FIELD(mb_baudrate, CFG_NUMBER),
    CFG_FIELD(mb_data_bits, CFG_NUMBER),
    CFG_FIELD(mb_stop_bits, CFG_NUMBER),
    CFG_FIELD(mb_parity, CFG_NUMBER),
    CFG_FIELD(mb_flow_ctrl, CFG_NUMBER),
    CFG_FIELD(mb_poll_interval_ms, CFG_NUMBER),
    CFG_FIELD(mq_broker_host, CFG_TEXT),
    CFG_FIELD(mq_broker_port, CFG_NUMBER),
    CFG_FIELD(mq_client_id, CFG_TEXT),
    CFG_FIELD(mq_username, CFG_TEXT),
    CFG_FIELD(mq_password, CFG_TEXT),
    CFG_FIELD(mq_pub_topic, CFG_TEXT),
    CFG_FIELD(mq_sub_topic, CFG_TEXT),
    CFG_FIELD(mq_report_interval_ms, CFG_NUMBER),
    {"mb_devices", 0U, 0U, CFG_DEVICES},
    CFG_FIELD(write_allowlist, CFG_ALLOW)
};
#undef CFG_FIELD

#define CFG_ITEM(type, member, name, kind) {name, offsetof(type, member), sizeof(((type *)0)->member), kind}
static const cfg_field_t s_device_fields[] =
{
    CFG_ITEM(modbus_t, u8id, "slave", CFG_NUMBER),
    CFG_ITEM(modbus_t, u8fct, "function", CFG_NUMBER),
    CFG_ITEM(modbus_t, u16RegAdd, "address", CFG_NUMBER),
    CFG_ITEM(modbus_t, u16CoilsNo, "count", CFG_NUMBER),
    CFG_ITEM(modbus_t, collect_interval_ms, "period_ms", CFG_NUMBER),
    CFG_ITEM(modbus_t, enable, "enabled", CFG_NUMBER),
    CFG_ITEM(modbus_t, retry_cnt, "retries", CFG_NUMBER),
    {"points", 0U, 0U, CFG_POINTS}
};
static const cfg_field_t s_point_fields[] =
{
    CFG_ITEM(mb_point_t, reg_index, "reg_index", CFG_NUMBER),
    CFG_ITEM(mb_point_t, data_type, "data_type", CFG_NUMBER),
    CFG_ITEM(mb_point_t, byte_order, "byte_order", CFG_NUMBER),
    CFG_ITEM(mb_point_t, scale, "scale", CFG_FLOAT),
    CFG_ITEM(mb_point_t, offset, "offset", CFG_FLOAT),
    CFG_ITEM(mb_point_t, name, "name", CFG_TEXT),
    CFG_ITEM(mb_point_t, unit, "unit", CFG_TEXT)
};
static const cfg_field_t s_rule_fields[] =
{
    CFG_ITEM(AppControlRule, slave, "slave", CFG_NUMBER),
    CFG_ITEM(AppControlRule, function, "function", CFG_NUMBER),
    CFG_ITEM(AppControlRule, start_address, "start_address", CFG_NUMBER),
    CFG_ITEM(AppControlRule, end_address, "end_address", CFG_NUMBER),
    CFG_ITEM(AppControlRule, min_value, "min_value", CFG_NUMBER),
    CFG_ITEM(AppControlRule, max_value, "max_value", CFG_NUMBER)
};
#undef CFG_ITEM
#define CFG_COUNT(fields) (sizeof(fields) / sizeof((fields)[0]))
#define CONFIG_FIELD_COUNT (sizeof(s_fields) / sizeof(s_fields[0]))
_Static_assert(CONFIG_FIELD_COUNT < 32U, "Config field bitmap too small");

static app_config_t s_candidate __attribute__((section(".bss.ccmram")));
static uint8_t s_json[STORAGE_CONFIG_CHUNK_MAX] __attribute__((section(".bss.ccmram")));
static mb_point_t s_points[MB_POINTS_PER_SLAVE] __attribute__((section(".bss.ccmram")));
static storage_config_message_t *s_io;
static uint32_t s_io_started;
static uint32_t s_read_size, s_read_offset, s_read_base, s_read_count, s_read_crc;
static uint32_t s_read_generation, s_read_expected_crc;
static bool s_io_failed, s_read_tf, s_install_points;
static AppConfigResult s_read_error;
static uint8_t s_device_ids[MB_SLAVE_HARD_MAX], s_device_count;
static uint8_t s_address_count;
static bool s_tf_requested;
static uint32_t s_tf_lease;
static HAL_StatusTypeDef cfg_io(storage_config_message_t *message, uint32_t timeout_ms);
static AppConfigResult s_last_result;
static bool s_restart_required;
static bool s_initialized;
static bool s_boot_checked;
static bool s_busy;
static bool s_paused;
static bool s_self_test;
static uint32_t s_session;
static uint32_t s_retry_ms;

// 累计读取块的CRC，整份结束后取反并与固定运行槽元数据比较。
static uint32_t cfg_crc32(uint32_t crc, const uint8_t *data, uint32_t size)
{
    for (uint32_t i = 0U; i < size; i++)
    {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++)
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
    return crc;
}

// 所有分块沿用本轮起点；进展和换块均不能续期。
static uint32_t cfg_remaining(void)
{
    uint32_t elapsed = (uint32_t)(HAL_GetTick() - s_io_started);
    return elapsed < CONFIG_IO_MS ? CONFIG_IO_MS - elapsed : 0U;
}

// 按需读取256 B，Config任务独占游标；失败后不把缺字节当作文件结束。
static uint8_t cfg_peek(void)
{
    if (s_io_failed || s_read_offset >= s_read_size) return 0U;
    if (s_read_offset >= s_read_base + s_read_count)
    {
        s_read_base = s_read_offset;
        s_io->action = s_read_tf ? STORAGE_CONFIG_TF_READ : STORAGE_CONFIG_READ_RUNNING;
        s_io->offset = s_read_base;
        s_io->len = s_read_size - s_read_base;
        if (s_io->len > sizeof(s_json)) s_io->len = sizeof(s_json);
        uint32_t wanted = s_io->len;
        HAL_StatusTypeDef result = s_read_tf && s_paused ? HAL_BUSY : cfg_io(s_io, cfg_remaining());
        if (result != HAL_OK || s_io->len != wanted ||
            (!s_read_tf && (s_io->total_len != s_read_size || s_io->generation != s_read_generation ||
                            s_io->crc32 != s_read_expected_crc)))
        {
            s_io_failed = true;
            s_read_error = result == HAL_TIMEOUT ? APP_CONFIG_TIMEOUT :
                           result == HAL_BUSY ? APP_CONFIG_BUSY : APP_CONFIG_STORAGE;
            return 0U;
        }
        s_read_count = wanted;
        memcpy(s_json, s_io->data, wanted);
        s_read_crc = cfg_crc32(s_read_crc, s_json, wanted);
    }
    return s_json[s_read_offset - s_read_base];
}

/** @brief 检查有界UTF-8字符串，拒绝未终止、控制字符和非法编码。 */
static bool cfg_text_valid(const char *text, uint32_t capacity, bool required)
{
    const uint8_t *p = (const uint8_t *)text;
    uint32_t i = 0;
    while (i < capacity && p[i] != 0U)
    {
        uint32_t first = p[i++];
        if (first < 0x80U)
        {
            if (first < 0x20U || first == 0x7FU) return false;
            continue;
        }
        uint32_t count = first >= 0xC2U && first <= 0xDFU ? 1U :
                         first >= 0xE0U && first <= 0xEFU ? 2U :
                         first >= 0xF0U && first <= 0xF4U ? 3U : 0U;
        if (count == 0U || count >= capacity - i) return false;
        uint32_t value = first & (0x7FU >> (count + 1U));
        for (uint32_t j = 0; j < count; j++)
        {
            uint32_t next = p[i++];
            if ((next & 0xC0U) != 0x80U) return false;
            value = (value << 6U) | (next & 0x3FU);
        }
        if ((count == 2U && value < 0x800U) || (count == 3U && value < 0x10000U) ||
            value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) return false;
    }
    return i < capacity && (!required || i != 0U);
}

// 校验基础参数、4站容量和写权限；完整JSON的点位约束在逐站解码时核对。
AppConfigResult AppConfig_Validate(const app_config_t *cfg)
{
    if (cfg == NULL || cfg->magic != CONFIG_MAGIC || cfg->version != CONFIG_VERSION || cfg->crc32 != 0U ||
        cfg->mb_slave_count > MB_SLAVE_HARD_MAX || cfg->mb_data_bits != 8U ||
        (cfg->mb_stop_bits != 1U && cfg->mb_stop_bits != 2U) || cfg->mb_parity > 2U ||
        (cfg->mb_parity != 0U && cfg->mb_stop_bits != 1U) || cfg->mb_flow_ctrl != 0U ||
        cfg->mb_poll_interval_ms < 100U || cfg->mb_poll_interval_ms > 3600000U ||
        cfg->mq_report_interval_ms < 1000U || cfg->mq_report_interval_ms > 3600000U ||
        cfg->mq_broker_port == 0U) return APP_CONFIG_INVALID;
    uint32_t baud = cfg->mb_baudrate;
    if (baud != 9600U && baud != 19200U && baud != 38400U &&
        baud != 57600U && baud != 115200U) return APP_CONFIG_INVALID;
    for (uint32_t i = 0; i < CONFIG_MAX_SLAVE; i++)
    {
        uint8_t address = cfg->mb_slave_addr[i];
        if (i >= cfg->mb_slave_count)
        {
            if (address != 0U) return APP_CONFIG_INVALID;
        }
        else
        {
            if (address == 0U || address > 247U) return APP_CONFIG_INVALID;
            for (uint32_t j = 0; j < i; j++)
                if (cfg->mb_slave_addr[j] == address) return APP_CONFIG_INVALID;
        }
    }
    for (uint32_t i = 0; i < CONFIG_FIELD_COUNT; i++)
    {
        const cfg_field_t *field = &s_fields[i];
        if (field->type == CFG_TEXT &&
            !cfg_text_valid((const char *)cfg + field->offset, field->size,
                            field->offset != offsetof(app_config_t, mq_username) &&
                            field->offset != offsetof(app_config_t, mq_password)))
            return APP_CONFIG_INVALID;
    }
    for (const char *p = cfg->mq_broker_host; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '.')) return APP_CONFIG_INVALID;
    for (const char *p = cfg->mq_client_id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return APP_CONFIG_INVALID;
    if ((cfg->mq_username[0] == '\0') != (cfg->mq_password[0] == '\0') ||
        strchr(cfg->mq_pub_topic, '#') != NULL || strchr(cfg->mq_pub_topic, '+') != NULL)
        return APP_CONFIG_INVALID;
    if (cfg->write_allowlist_count > CONFIG_WRITE_MAX) return APP_CONFIG_INVALID;
    for (uint32_t i = 0U; i < cfg->write_allowlist_count; i++)
    {
        const AppControlRule *rule = &cfg->write_allowlist[i];
        bool coil = rule->function == 5U || rule->function == 15U;
        bool found = false;
        for (uint32_t j = 0U; j < cfg->mb_slave_count; j++)
            if (cfg->mb_slave_addr[j] == rule->slave) found = true;
        if (!found || (!coil && rule->function != 6U && rule->function != 16U) ||
            rule->start_address > rule->end_address || rule->min_value > rule->max_value ||
            (coil && rule->max_value > 1U)) return APP_CONFIG_INVALID;
    }
    return APP_CONFIG_OK;
}

// 跳过JSON空白，嵌入NUL不会被视作合法尾部。
static void cfg_space(void)
{
    while (cfg_peek() == ' ' || cfg_peek() == '\t' || cfg_peek() == '\r' || cfg_peek() == '\n')
        s_read_offset++;
}

// 消费指定分隔符，I/O失败或EOF不能匹配成功。
static bool cfg_expect(char value)
{
    cfg_space();
    if (s_io_failed || s_read_offset >= s_read_size || cfg_peek() != (uint8_t)value) return false;
    s_read_offset++;
    return true;
}

// 读取有界UTF-8字符串，沿用引号、斜杠和反斜杠转义，不接受控制字符。
static bool cfg_get_text(char *out, uint32_t capacity)
{
    if (!cfg_expect('"')) return false;
    uint32_t used = 0U;
    while (cfg_peek() != 0U && cfg_peek() != '"')
    {
        uint8_t value = cfg_peek();
        s_read_offset++;
        if (value == '\\')
        {
            value = cfg_peek();
            if (value != '"' && value != '\\' && value != '/') return false;
            s_read_offset++;
        }
        if (value < 0x20U || used + 1U >= capacity) return false;
        out[used++] = (char)value;
    }
    if (s_io_failed || cfg_peek() != '"') return false;
    s_read_offset++;
    out[used] = '\0';
    return cfg_text_valid(out, capacity, false);
}

// 严格无符号整数，拒绝前导零和溢出；游标可跨输入块。
static bool cfg_get_number(uint32_t *value)
{
    cfg_space();
    uint32_t started = s_read_offset;
    uint8_t first = cfg_peek();
    if (first < '0' || first > '9') return false;
    *value = 0U;
    while (cfg_peek() >= '0' && cfg_peek() <= '9')
    {
        uint32_t digit = cfg_peek() - '0';
        if (*value > (UINT32_MAX - digit) / 10U) return false;
        *value = *value * 10U + digit;
        s_read_offset++;
    }
    return !s_io_failed && (first != '0' || s_read_offset == started + 1U);
}

// 小数和指数只用于float32换算参数，不调用带动态工作区的strtod。
static bool cfg_get_float(float *out)
{
    cfg_space();
    bool negative = cfg_peek() == '-';
    if (negative) s_read_offset++;
    uint8_t first = cfg_peek();
    if (first < '0' || first > '9') return false;
    double value = 0.0;
    uint32_t digits = 0U;
    while (cfg_peek() >= '0' && cfg_peek() <= '9')
    {
        if (++digits > 40U) return false;
        value = value * 10.0 + (cfg_peek() - '0');
        s_read_offset++;
    }
    if (first == '0' && digits != 1U) return false;
    int exponent = 0;
    if (cfg_peek() == '.')
    {
        s_read_offset++;
        uint32_t fraction = 0U;
        while (cfg_peek() >= '0' && cfg_peek() <= '9')
        {
            if (++digits > 48U) return false;
            value = value * 10.0 + (cfg_peek() - '0');
            s_read_offset++;
            exponent--;
            fraction++;
        }
        if (fraction == 0U) return false;
    }
    if (cfg_peek() == 'e' || cfg_peek() == 'E')
    {
        s_read_offset++;
        bool minus = cfg_peek() == '-';
        if (minus || cfg_peek() == '+') s_read_offset++;
        uint32_t magnitude = 0U, count = 0U;
        while (cfg_peek() >= '0' && cfg_peek() <= '9')
        {
            if (++count > 3U) return false;
            magnitude = magnitude * 10U + cfg_peek() - '0';
            s_read_offset++;
        }
        if (count == 0U || magnitude > 90U) return false;
        exponent += minus ? -(int)magnitude : (int)magnitude;
    }
    while (exponent > 0) { value *= 10.0; exponent--; }
    while (exponent < 0) { value /= 10.0; exponent++; }
    *out = (float)(negative ? -value : value);
    return !s_io_failed && isfinite(*out) && (value == 0.0 || *out != 0.0f);
}

static bool cfg_get_fields(void *object, const cfg_field_t *fields, uint32_t count);

// 点位只使用这一站的暂存表，整份配置校验通过后才在第二遍安装。
static bool cfg_get_points(void)
{
    memset(s_points, 0, sizeof(s_points));
    if (!cfg_expect('[')) return false;
    uint32_t count = 0U;
    cfg_space();
    if (cfg_peek() != ']') for (;;)
    {
        if (count == MB_POINTS_PER_SLAVE ||
            !cfg_get_fields(&s_points[count], s_point_fields, CFG_COUNT(s_point_fields))) return false;
        s_points[count++].valid = 1U;
        cfg_space();
        if (cfg_peek() != ',') break;
        s_read_offset++;
    }
    return cfg_expect(']');
}

// 验证寄存器跨度、类型、字节序及点名；启动安装不经过普通运行期变更入口。
static bool cfg_get_devices(void)
{
    if (!cfg_expect('[')) return false;
    s_device_count = 0U;
    cfg_space();
    if (cfg_peek() != ']') for (;;)
    {
        modbus_t device = {0};
        if (s_device_count == MB_SLAVE_HARD_MAX ||
            !cfg_get_fields(&device, s_device_fields, CFG_COUNT(s_device_fields))) return false;
        if (device.u8id == 0U || device.u8id > 247U ||
            (device.u8fct != MB_FC_READ_REGISTERS && device.u8fct != MB_FC_READ_INPUT_REGISTER) ||
            device.u16CoilsNo == 0U || device.u16CoilsNo > MB_REG_MAX_PER_SLAVE ||
            (uint32_t)device.u16RegAdd + device.u16CoilsNo > 65536U ||
            device.enable > 1U || device.retry_cnt > 3U ||
            device.collect_interval_ms < 100U || device.collect_interval_ms > 3600000U) return false;
        uint32_t points = 0U;
        for (uint32_t j = 0U; j < MB_POINTS_PER_SLAVE && s_points[j].valid != 0U; j++)
        {
            const mb_point_t *point = &s_points[j];
            uint32_t type = (uint32_t)point->data_type;
            if (type > MB_DT_FLOAT32 ||
                point->reg_index + (type <= MB_DT_INT16 ? 1U : 2U) > device.u16CoilsNo ||
                point->byte_order > (type <= MB_DT_INT16 ? 1U : 3U) ||
                !isfinite(point->scale) || !isfinite(point->offset) || point->name[0] == '\0' ||
                strchr(point->unit, '"') != NULL || strchr(point->unit, '\\') != NULL) return false;
            for (const char *p = point->name; *p; p++)
                if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
            for (uint32_t k = 0U; k < j; k++)
                if (strcmp(point->name, s_points[k].name) == 0) return false;
            points++;
        }
        if ((device.enable && points == 0U) ||
            (s_install_points && !Modbus_LoadSlave(&device, s_points))) return false;
        s_device_ids[s_device_count++] = device.u8id;
        cfg_space();
        if (cfg_peek() != ',') break;
        s_read_offset++;
    }
    return cfg_expect(']');
}

// 复用既有字段描述，严格拒绝未知、重复、缺失字段，不创建通用JSON树。
static bool cfg_get_fields(void *object, const cfg_field_t *fields, uint32_t count)
{
    if (!cfg_expect('{')) return false;
    uint32_t seen = 0U;
    for (;;)
    {
        char name[32];
        if (!cfg_get_text(name, sizeof(name)) || !cfg_expect(':')) return false;
        uint32_t index = 0U;
        while (index < count && strcmp(name, fields[index].name) != 0) index++;
        if (index == count || (seen & (1UL << index)) != 0U) return false;
        seen |= 1UL << index;
        const cfg_field_t *field = &fields[index];
        uint8_t *out = (uint8_t *)object + field->offset;
        if (field->type == CFG_TEXT)
        {
            if (!cfg_get_text((char *)out, field->size)) return false;
        }
        else if (field->type == CFG_NUMBER)
        {
            uint32_t value;
            if (!cfg_get_number(&value) || (field->size == 1U && value > UINT8_MAX) ||
                (field->size == 2U && value > UINT16_MAX)) return false;
            memcpy(out, &value, field->size);
        }
        else if (field->type == CFG_FLOAT)
        {
            if (!cfg_get_float((float *)out)) return false;
        }
        else if (field->type == CFG_DEVICES)
        {
            if (!cfg_get_devices()) return false;
        }
        else if (field->type == CFG_POINTS)
        {
            if (!cfg_get_points()) return false;
        }
        else
        {
            uint32_t total = 0U;
            uint32_t limit = field->type == CFG_ALLOW ? CONFIG_WRITE_MAX : MB_SLAVE_HARD_MAX;
            if (!cfg_expect('[')) return false;
            cfg_space();
            if (cfg_peek() != ']') for (;;)
            {
                if (total == limit) return false;
                if (field->type == CFG_ALLOW)
                {
                    if (!cfg_get_fields(out + total * sizeof(AppControlRule),
                                        s_rule_fields, CFG_COUNT(s_rule_fields))) return false;
                }
                else
                {
                    uint32_t value;
                    if (!cfg_get_number(&value) || value > 247U) return false;
                    out[total] = (uint8_t)value;
                }
                total++;
                cfg_space();
                if (cfg_peek() != ',') break;
                s_read_offset++;
            }
            if (!cfg_expect(']')) return false;
            if (field->type == CFG_ALLOW) ((app_config_t *)object)->write_allowlist_count = (uint8_t)total;
            else s_address_count = (uint8_t)total;
        }
        cfg_space();
        if (cfg_peek() != ',') break;
        s_read_offset++;
    }
    return cfg_expect('}') && seen == ((1UL << count) - 1U);
}

// 完整消费输入并核对从站列表，失败时暂存结果不能发布为运行配置。
static bool cfg_decode(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = CONFIG_MAGIC;
    s_read_offset = s_read_base = s_read_count = 0U;
    s_read_crc = UINT32_MAX;
    s_address_count = s_device_count = 0U;
    s_io_failed = false;
    s_read_error = APP_CONFIG_OK;
    if (s_read_size == 0U || s_read_size > CONFIG_JSON_MAX ||
        !cfg_get_fields(cfg, s_fields, CONFIG_FIELD_COUNT)) return false;
    cfg_space();
    if (s_io_failed || s_read_offset != s_read_size || s_address_count != cfg->mb_slave_count ||
        s_device_count != cfg->mb_slave_count || AppConfig_Validate(cfg) != APP_CONFIG_OK) return false;
    for (uint32_t i = 0U; i < s_device_count; i++)
        if (s_device_ids[i] != cfg->mb_slave_addr[i]) return false;
    return true;
}

/** @brief 请求值拷贝存储操作，阻塞前声明原期限，返回后报告真实进展。 */
static HAL_StatusTypeDef cfg_io(storage_config_message_t *message, uint32_t timeout_ms)
{
    if (timeout_ms == 0U || !Monitor_BeginWait(APP_MODULE_CONFIG, timeout_ms + 1000U)) return HAL_TIMEOUT;
    HAL_StatusTypeDef result = storage_runtime_config(message, timeout_ms);
    if (!Monitor_ReportProgress(APP_MODULE_CONFIG, s_self_test)) return HAL_ERROR;
    return result;
}

// 实体按键只投递一次请求，文件I/O和解析仍由原Config任务完成。
bool AppConfig_RequestTfImport(void)
{
    if (__get_IPSR() != 0U || !App_IsCurrentModuleTask(APP_MODULE_UPGRADE)) return false;
    taskENTER_CRITICAL();
    bool accepted = s_initialized && s_boot_checked && s_self_test && !s_paused && !s_busy &&
                    !s_tf_requested && s_session == 0U && s_tf_lease == 0U &&
                    UpgradeQuiesce_AllowsMutation();
    if (accepted)
    {
        s_tf_requested = true;
        s_last_result = APP_CONFIG_BUSY;
    }
    taskEXIT_CRITICAL();
    return accepted;
}

/*
 * Config 的 pause 回调：关闭新候选准入，取消尚未开始的 TF 导入请求。
 * 已开始的保存、存储 RPC 和文件操作仍由原 Config_Task 结算，返回成功不代表在途处理已结束。
 */
static UpgradeStageResult cfg_pause(void *context, uint32_t timeout_ms)
{
    (void)context;
    if (timeout_ms == 0U) return UPGRADE_STAGE_TIMEOUT;
    taskENTER_CRITICAL();
    s_paused = true;
    if (s_tf_requested)
    {
        // 只撤销待处理请求，已经开始的事务身份由原任务保留并完成结算。
        s_tf_requested = false;
        s_last_result = APP_CONFIG_BUSY;
    }
    bool ready = s_boot_checked && s_self_test;
    taskEXIT_CRITICAL();
    return ready ? UPGRADE_STAGE_OK : UPGRADE_STAGE_NOT_READY;
}

/*
 * 等待原配置任务归还候选事务和 TF 文件租约，RPC、提交核对或关闭未完成时不能确认停稳。
 * 只检查真实状态；超时不清占用，也不把未保存的结果当成已完成。
 */
static UpgradeStageResult cfg_settle(void *context, uint32_t timeout_ms)
{
    (void)context;
    uint32_t started = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - started) < timeout_ms)
    {
        taskENTER_CRITICAL();
        bool settled = s_paused && !s_busy && !s_tf_requested && s_session == 0U && s_tf_lease == 0U;
        taskEXIT_CRITICAL();
        if (settled) return UPGRADE_STAGE_OK;
        if (osDelay(1U) != osOK) return UPGRADE_STAGE_INTERNAL;
    }
    return UPGRADE_STAGE_TIMEOUT;
}

// 结算成功后撤销 Config 本地暂停，统一变更入口仍由协调器在所有模块恢复后最后开放。
static UpgradeStageResult cfg_resume(void *context, uint32_t timeout_ms)
{
    UpgradeStageResult result = cfg_settle(context, timeout_ms);
    if (result == UPGRADE_STAGE_OK)
    {
        taskENTER_CRITICAL();
        s_paused = false;
        taskEXIT_CRITICAL();
    }
    return result;
}

/** @brief 只读核对提交或取消原租约；未知结果保留占用，不接纳下一份候选。 */
static void cfg_cleanup(storage_config_message_t *message)
{
    memset(message, 0, sizeof(*message));
    message->action = STORAGE_CONFIG_RESOLVE;
    message->session_id = s_session;
    HAL_StatusTypeDef result = cfg_io(message, 3000U);
    if (result == HAL_OK && message->generation != 0U)
    {
        taskENTER_CRITICAL();
        s_restart_required = true;
        s_last_result = APP_CONFIG_OK;
        s_session = 0U;
        taskEXIT_CRITICAL();
        return;
    }
    message->action = STORAGE_CONFIG_ABORT;
    result = cfg_io(message, 3000U);
    if (result == HAL_OK)
    {
        taskENTER_CRITICAL();
        s_session = 0U;
        taskEXIT_CRITICAL();
    }
}

// 启动先验证整份JSON及CRC，再第二遍加载到现有Modbus表，最后发布运行快照。
static void cfg_load(storage_config_message_t *message)
{
    memset(message, 0, sizeof(*message));
    message->action = STORAGE_CONFIG_READ_RUNNING;
    message->len = 1U;
    s_io = message;
    s_io_started = HAL_GetTick();
    if (cfg_io(message, 3000U) != HAL_OK)
    {
        s_last_result = APP_CONFIG_STORAGE;
        return;
    }
    s_read_size = message->total_len;
    s_read_generation = message->generation;
    s_read_expected_crc = message->crc32;
    s_read_tf = s_install_points = false;
    bool valid = cfg_decode(&s_candidate) && ~s_read_crc == s_read_expected_crc &&
                 Modbus_LoadConfig(&s_candidate);
    if (valid)
    {
        s_install_points = true;
        valid = cfg_decode(&s_candidate) && ~s_read_crc == s_read_expected_crc &&
                AppConfig_Activate(&s_candidate);
        s_install_points = false;
    }
    taskENTER_CRITICAL();
    s_boot_checked = true;
    s_self_test = s_read_size == 0U || valid;
    s_last_result = s_self_test ? APP_CONFIG_OK : APP_CONFIG_INVALID;
    taskEXIT_CRITICAL();
    (void)App_SetResourceReady(APP_RESOURCE_CONFIG, valid);
}

// 只有明确关闭文件后才释放租约，迟到OPEN仍须以原号关闭。
static bool cfg_close_tf(storage_config_message_t *message)
{
    if (s_tf_lease == 0U) return true;
    memset(message, 0, sizeof(*message));
    message->action = STORAGE_CONFIG_TF_CLOSE;
    message->session_id = s_tf_lease;
    if (cfg_io(message, 3000U) != HAL_OK) return false;
    s_tf_lease = 0U;
    return true;
}

// 将已校验的TF原文写入候选槽，两遍CRC一致且文件关闭后才提交。
static void cfg_save(storage_config_message_t *message)
{
    memset(message, 0, sizeof(*message));
    s_io = message;
    uint32_t expected_crc = ~s_read_crc;
    uint32_t total = s_read_size;
    message->action = STORAGE_CONFIG_BEGIN;
    message->total_len = total;
    HAL_StatusTypeDef result = cfg_io(message, cfg_remaining());
    s_session = message->session_id;
    uint32_t crc = UINT32_MAX;
    for (uint32_t offset = 0U; result == HAL_OK && offset < total;)
    {
        if (s_paused || !UpgradeQuiesce_AllowsMutation()) { result = HAL_BUSY; break; }
        message->action = STORAGE_CONFIG_TF_READ;
        message->session_id = s_tf_lease;
        message->offset = offset;
        message->len = total - offset;
        if (message->len > sizeof(message->data)) message->len = sizeof(message->data);
        result = cfg_io(message, cfg_remaining());
        if (result != HAL_OK) break;
        crc = cfg_crc32(crc, message->data, message->len);
        message->action = STORAGE_CONFIG_CHUNK;
        message->session_id = s_session;
        result = cfg_io(message, cfg_remaining());
        if (result == HAL_OK) offset += message->len;
    }
    if (~crc != expected_crc || !cfg_close_tf(message)) result = HAL_ERROR;
    message->session_id = s_session;
    for (uint32_t action = STORAGE_CONFIG_VALIDATE; result == HAL_OK && action <= STORAGE_CONFIG_COMMIT; action++)
    {
        if (s_paused || !UpgradeQuiesce_AllowsMutation()) { result = HAL_BUSY; break; }
        message->action = (storage_config_action_t)action;
        result = cfg_io(message, cfg_remaining());
    }
    taskENTER_CRITICAL();
    s_last_result = result == HAL_OK ? APP_CONFIG_OK :
                           result == HAL_TIMEOUT ? APP_CONFIG_TIMEOUT : APP_CONFIG_STORAGE;
    if (result == HAL_OK)
    {
        s_restart_required = true;
        s_session = 0U;
    }
    taskEXIT_CRITICAL();
    if (s_session != 0U) cfg_cleanup(message);
}

// 显式TF请求先完整验证，再保存原文；失败保留当前运行R和上次保存S。
static void cfg_import_tf(storage_config_message_t *message)
{
    memset(message, 0, sizeof(*message));
    message->action = STORAGE_CONFIG_TF_OPEN;
    s_io = message;
    s_io_started = HAL_GetTick();
    HAL_StatusTypeDef result = cfg_io(message, 3000U);
    s_tf_lease = message->session_id;
    s_read_size = message->total_len;
    s_read_tf = true;
    s_install_points = false;
    bool valid = result == HAL_OK && cfg_decode(&s_candidate);
    if (valid) cfg_save(message);
    else s_last_result = result == HAL_TIMEOUT ? APP_CONFIG_TIMEOUT :
                               result != HAL_OK ? APP_CONFIG_STORAGE :
                               s_read_error != APP_CONFIG_OK ? s_read_error : APP_CONFIG_INVALID;
    if (!cfg_close_tf(message)) s_last_result = APP_CONFIG_STORAGE;
    s_read_tf = false;
    printf("[CFG] TF import result=%u restart=%u\r\n",
           (unsigned)s_last_result, s_restart_required);
}

/** @brief 绑定真实Config暂停角色；不访问介质，不创建额外任务或队列。 */
AppModuleInitResult Config_Init(void)
{
    if (s_initialized) return APP_MODULE_INIT_OK;
    // sync 复用 cfg_settle 复核安全点；Config 不直接刷介质，后续由 Storage 所有者执行同步。
    const UpgradeQuiesceParticipant participant =
    {
        cfg_pause, cfg_settle, cfg_settle, cfg_resume, NULL
    };
    if (UpgradeQuiesce_Bind(UPGRADE_QUIESCE_CONFIG, &participant) != UPGRADE_STAGE_OK)
        return APP_MODULE_INIT_FAILED;
    s_initialized = true;
    s_last_result = APP_CONFIG_NOT_READY;
    return APP_MODULE_INIT_OK;
}

/** @brief 现有Config任务负责加载、保存及取消结算，空闲时检查真实状态并报告监督。 */
void Config_Task(void *argument)
{
    (void)argument;
    storage_config_message_t message;
    if (!s_initialized || !Monitor_RegisterHealth(APP_MODULE_CONFIG, 2000U, CONFIG_IO_MS + 1000U))
        goto failed;
    for (;;)
    {
        if (!Monitor_ReportProgress(APP_MODULE_CONFIG, s_self_test)) goto failed;
        if (!s_boot_checked && App_IsReady(APP_RESOURCE_CONTROL_STATE)) cfg_load(&message);
        taskENTER_CRITICAL();
        bool import = s_tf_requested && !s_paused;
        if (import) { s_tf_requested = false; s_busy = true; }
        taskEXIT_CRITICAL();
        if (import)
        {
            cfg_import_tf(&message);
            taskENTER_CRITICAL();
            s_busy = false;
            taskEXIT_CRITICAL();
        }
        else if ((s_session != 0U || s_tf_lease != 0U) && (uint32_t)(HAL_GetTick() - s_retry_ms) >= 1000U)
        {
            s_retry_ms = HAL_GetTick();
            if (s_tf_lease != 0U) (void)cfg_close_tf(&message);
            if (s_session != 0U) cfg_cleanup(&message);
        }
        s_io = NULL;
        if (!Monitor_ReportProgress(APP_MODULE_CONFIG, s_self_test) ||
            !Monitor_BeginWait(APP_MODULE_CONFIG, CONFIG_POLL_MS + 1000U) ||
            osDelay((osKernelGetTickFreq() * CONFIG_POLL_MS + 999U) / 1000U) != osOK) goto failed;
    }
failed:
    Monitor_ReportFatal(APP_MODULE_CONFIG);
    osThreadExit();
}

/* ===== MQTT 下发配置变更命令处理（运行时即时新增/删除/修改从站）===== */

/* 字符串转数据类型枚举 */
static mb_data_type_t parse_data_type(const char *type_str)
{
    if (!type_str) return MB_DT_UINT16;
    if (strcmp(type_str, "uint16")  == 0) return MB_DT_UINT16;
    if (strcmp(type_str, "int16")   == 0) return MB_DT_INT16;
    if (strcmp(type_str, "uint32")  == 0) return MB_DT_UINT32;
    if (strcmp(type_str, "int32")   == 0) return MB_DT_INT32;
    if (strcmp(type_str, "float32") == 0) return MB_DT_FLOAT32;
    if (strcmp(type_str, "bit")     == 0) return MB_DT_BIT;
    printf("[CFG] unknown type '%s', default uint16\r\n", type_str);
    return MB_DT_UINT16;
}

/* 从 JSON 节点解析一个点位到 mb_point_t */
static bool parse_one_point(const mini_json_t *slave_pts, mb_point_t *out_point)
{
    memset(out_point, 0, sizeof(mb_point_t));
    out_point->reg_index = (uint16_t)MiniJson_GetInt(slave_pts, "reg_index", 0);
    const char *type_str = MiniJson_GetString(slave_pts, "type", "uint16");
    out_point->data_type = parse_data_type(type_str);
    out_point->scale  = (float)MiniJson_GetFloat(slave_pts, "scale", 1.0);
    out_point->offset = (float)MiniJson_GetFloat(slave_pts, "offset", 0.0);
    const char *name = MiniJson_GetString(slave_pts, "name", "");
    strncpy(out_point->name, name, MB_POINT_NAME_MAX - 1);
    out_point->name[MB_POINT_NAME_MAX - 1] = '\0';
    const char *unit = MiniJson_GetString(slave_pts, "unit", "");
    strncpy(out_point->unit, unit, MB_POINT_UNIT_MAX - 1);
    out_point->unit[MB_POINT_UNIT_MAX - 1] = '\0';
    out_point->bit_index = (uint8_t)MiniJson_GetInt(slave_pts, "bit_index", 0);
    out_point->valid = 1;
    return true;
}

/* 从 JSON 节点解析一个从站并调 mb_apply_slave_config（UPSERT） */
static int apply_one_slave(const mini_json_t *slave_node)
{
    if (!slave_node) return -1;
    int id = MiniJson_GetInt(slave_node, "id", 0);
    if (id < 1 || id > 247) { printf("[CFG] invalid slave id %d\r\n", id); return -1; }
    int fc = MiniJson_GetInt(slave_node, "fc", 3);
    if (fc < 1 || fc > 4) { printf("[CFG] invalid fc %d, default 3\r\n", fc); fc = 3; }
    int reg = MiniJson_GetInt(slave_node, "reg", 0);
    int count = MiniJson_GetInt(slave_node, "count", 1);
    if (count < 1 || count > 32) { printf("[CFG] invalid count %d, clamp to 1\r\n", count); count = 1; }
    int interval = MiniJson_GetInt(slave_node, "interval", 2000);
    bool enable = MiniJson_GetBool(slave_node, "enable", true);
    int retry = MiniJson_GetInt(slave_node, "retry", 1);

    modbus_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.u8id               = (uint8_t)id;
    cfg.u8fct              = (mb_functioncode_t)fc;
    cfg.u16RegAdd          = (uint16_t)reg;
    cfg.u16CoilsNo         = (uint16_t)count;
    cfg.u16reg             = NULL;
    cfg.u32CurrentTask     = NULL;
    cfg.collect_interval_ms = (uint32_t)interval;
    cfg.enable             = enable ? 1 : 0;
    cfg.retry_cnt          = (uint8_t)retry;

    mb_point_t points[MB_POINTS_PER_SLAVE];
    memset(points, 0, sizeof(points));
    mini_json_t *pts_arr = MiniJson_ObjectGet(slave_node, "points");
    int pts_count = 0;
    if (pts_arr && pts_arr->type == MJ_TYPE_ARRAY)
    {
        int arr_size = MiniJson_ArraySize(pts_arr);
        for (int i = 0; i < arr_size && i < MB_POINTS_PER_SLAVE; i++)
        {
            mini_json_t *pt_node = MiniJson_ArrayGet(pts_arr, i);
            if (pt_node) { parse_one_point(pt_node, &points[i]); pts_count++; }
        }
    }

    printf("[CFG] apply slave id=%d fc=%d reg=%d count=%d pts=%d\r\n",
           id, fc, reg, count, pts_count);

    mb_status_e st = mb_apply_slave_config(MB_CFG_OP_UPSERT, (uint8_t)id, &cfg, points);
    if (st != MB_STATUS_OK)
    {
        printf("[CFG] mb_apply_slave_config failed slave=%d st=%d\r\n", id, (int)st);
        return -2;
    }
    return 0;
}

/* 从 JSON 节点删除一个从站（DELETE） */
static int delete_one_slave(const mini_json_t *slave_node)
{
    if (!slave_node) return -1;
    int id = MiniJson_GetInt(slave_node, "id", 0);
    if (id < 1 || id > 247) { printf("[CFG] delete: invalid slave id %d\r\n", id); return -1; }
    printf("[CFG] delete slave id=%d\r\n", id);
    mb_status_e st = mb_apply_slave_config(MB_CFG_OP_DELETE, (uint8_t)id, NULL, NULL);
    if (st != MB_STATUS_OK)
    {
        printf("[CFG] delete failed slave=%d st=%d\r\n", id, (int)st);
        return -2;
    }
    return 0;
}

/* 解析完整配置变更 JSON 并应用到 M1
 * 返回 0=成功  -1=JSON解析失败  -2=升级静默期拒绝  -3=部分从站应用失败 */
static int config_apply_json(const char *json_str)
{
    if (!json_str || !*json_str) return -1;
    if (!UpgradeQuiesce_AllowsMutation())
    {
        printf("[CFG] rejected: upgrade quiesce\r\n");
        return -2;
    }
    mini_json_t *root = MiniJson_Parse(json_str);
    if (!root) { printf("[CFG] json parse failed\r\n"); return -1; }

    const char *op = MiniJson_GetString(root, "op", "upsert");
    mini_json_t *slaves = MiniJson_ObjectGet(root, "slaves");
    if (!slaves || slaves->type != MJ_TYPE_ARRAY)
    {
        printf("[CFG] no slaves array\r\n");
        MiniJson_Free(root);
        return -1;
    }

    int arr_size = MiniJson_ArraySize(slaves);
    int ok_count = 0, fail_count = 0;
    mb_config_batch_begin();

    if (strcmp(op, "delete") == 0)
    {
        for (int i = 0; i < arr_size; i++)
        {
            mini_json_t *s = MiniJson_ArrayGet(slaves, i);
            int r = delete_one_slave(s);
            if (r == 0) ok_count++; else fail_count++;
        }
    }
    else
    {
        for (int i = 0; i < arr_size; i++)
        {
            mini_json_t *s = MiniJson_ArrayGet(slaves, i);
            int r = apply_one_slave(s);
            if (r == 0) ok_count++; else fail_count++;
        }
    }

    mb_config_batch_end();
    printf("[CFG] done: op=%s ok=%d fail=%d\r\n", op, ok_count, fail_count);
    MiniJson_Free(root);

    if (fail_count > 0) return -3;
    return 0;
}

/* 对外接口：处理 MQTT 下发的配置变更 JSON */
int Config_HandleMessage(const char *json_buf)
{
    return config_apply_json(json_buf);
}
