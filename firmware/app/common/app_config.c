/*
 * app_config.c —— 应用全局配置实现
 *
 * 功能：
 *   1. 提供默认配置（上电时 Flash 里没存配置就用这个）
 *   2. 从 Flash 加载配置（M3 Storage 模块提供）
 *   3. 保存配置到 Flash
 *
 * 设计：
 *   - 默认值集中在这里，改参数只改这一处
 *   - 配置存在 SPI Flash（W25Q128）里，掉电不丢
 *   - 上电先读 Flash，CRC 校验不对就用默认值
 */
#include "app_config.h"
#include "upgrade_quiesce.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "stm32f4xx.h"
#include <string.h>

/* 默认配置：第一次上电、或者 Flash 里配置损坏时用这个 */
static const app_config_t s_default_config = {
    /* 公共头 */
    .magic = CONFIG_MAGIC,
    .version = CONFIG_VERSION,
    .crc32 = 0,  /* CRC 由 Storage 模块算 */

    /* Modbus 参数（M1） */
    .mb_slave_count = 4,
    .mb_slave_addr = {1, 2, 3, 4},
    .mb_baudrate = 9600,              /* 匹配传感器出厂默认（B-RS-L30/TAS-SJ-R 均 9600） */
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

/* 全局配置实例：上电后从 Flash 加载，或者用默认值 */
static app_config_t s_config;

/**
 * @brief  获取默认配置（上电时用）
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
    return &s_config;
}

/**
 * @brief  设置当前配置（从 Flash 加载完调用）
 * @param  cfg 新配置
 * @return false表示空参数或变更门控关闭；true仅表示RAM更新完成
 */
bool AppConfig_Set(const app_config_t *cfg)
{
    if (cfg == NULL || __get_IPSR() != 0U || osKernelGetState() != osKernelRunning)
        return false;
    taskENTER_CRITICAL();
    bool allowed = UpgradeQuiesce_AllowsMutation();
    if (allowed) memcpy(&s_config, cfg, sizeof(app_config_t));
    taskEXIT_CRITICAL();
    return allowed;
}
