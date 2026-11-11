/**
 * @file mb_pointmap.c
 * @brief Modbus 点位表配置实现 —— 寄存器值 → 物理量
 *
 *  核心换算：physical = raw * scale + offset
 *
 *  多字节寄存器拼接严格大端序：高寄存器在前（reg[i]<<16 | reg[i+1]）
 *  float32 通过 memcpy 走，规避严格别名规则。
 *
 *  本文件依赖 mb_api.h（访问 slave_list / 锁 / cfg_change_cb）。
 */

#include "mb_pointmap.h"
#include "mb_internal.h"
#include <string.h>

/* ========= 全局上报模式 ========= */
static mb_report_mode_t s_report_mode = MB_REPORT_PARSED;

/* ========= 内部辅助：单寄存器类型拼值 + 换算 ========= */
/**
 * @brief 从 reg_data_buf[] 取出一个物理值
 *
 * @param reg      寄存器数组起点（reg_data_buf[]）
 * @param reg_len  该从站有效寄存器数（做边界检查用）
 * @param idx      点位 reg_index（reg 内起始下标）
 * @param type     数据类型
 * @param bit_idx  位号 0~15（仅 MB_DT_BIT 使用）
 * @return 换算前的原始值（int64_t 中间态，大类型能装下）
 */
static double mb_pointmap_raw_value(const uint16_t *reg, uint16_t reg_len,
                                    uint16_t idx, mb_data_type_t type,
                                    uint8_t bit_idx)
{
    uint8_t need = mb_dt_reg_count[type];
    if (idx + need > reg_len)
    {
        return 0.0;  // 越界，返回 0
    }

    switch (type)
    {
        case MB_DT_UINT16:
            return (double)reg[idx];

        case MB_DT_INT16:
            return (double)(int16_t)reg[idx];

        case MB_DT_BIT:
            // 位提取：FC01/02 线圈打包字，或寄存器里的状态位
            // Modbus 标准：线圈 N → 第 N/16 个字的 bit (N%16)，字节内 LSB 在前
            if (bit_idx > 15) return 0.0;
            return (double)((reg[idx] >> bit_idx) & 0x01);

        case MB_DT_UINT32:
            return (double)(((uint32_t)reg[idx] << 16) | reg[idx + 1]);

        case MB_DT_INT32:
            return (double)(int32_t)(((uint32_t)reg[idx] << 16) | reg[idx + 1]);

        case MB_DT_FLOAT32:
        {
            uint32_t raw32 = ((uint32_t)reg[idx] << 16) | reg[idx + 1];
            float f;
            memcpy(&f, &raw32, sizeof(f));   // 规避严格别名
            return (double)f;
        }

        default:
            return 0.0;
    }
}

/* ========= 设置 / 获取 点位配置 ========= */

/**
 * @brief 设置点位表核心逻辑（不加锁，调用方必须已持 xSlaveCfgMutex）
 */
int mb_pointmap_set_slave_points_nolock(uint8_t slave_id, const mb_point_t *points)
{
    mb_handle_t *h = mb_handle_get();
    if (h == NULL) return MB_HANDLE_STATUS_ERR;
    if (points == NULL) return MB_BUILD_REQUEST_ERR;

    mb_slave_runtime_t *s = mb_get_slave_runtime(slave_id);
    if (s == NULL) return MB_SLAVE_NOT_FOUND_ERR;

    memcpy(&s->points, points, sizeof(s->points));
    return MB_STATUS_OK;
}

/**
 * @brief 设置点位表（公开加锁版）
 */
static int mb_pointmap_set_slave_points_impl(uint8_t slave_id, const mb_point_t *points)
{
    if (points == NULL) return MB_BUILD_REQUEST_ERR;

    mb_handle_t *h = mb_handle_get();
    if (h == NULL) return MB_HANDLE_STATUS_ERR;

    osMutexAcquire(h->xSlaveCfgMutex, osWaitForever);
    int ret = mb_pointmap_set_slave_points_nolock(slave_id, points);
    osMutexRelease(h->xSlaveCfgMutex);

    if (ret == MB_STATUS_OK)
        mb_fire_cfg_change(MB_CFG_CHANGE_UPDATE, slave_id);

    return ret;
}

int mb_pointmap_get_slave_points(uint8_t slave_id, mb_point_t *out)
{
    if (out == NULL) return -1;

    mb_handle_t *h = mb_handle_get();
    if (h == NULL) return -1;

    osMutexAcquire(h->xSlaveCfgMutex, osWaitForever);

    mb_slave_runtime_t *s = NULL;
    for (uint8_t i = 0; i < h->slave_max_num; i++)
    {
        mb_slave_runtime_t *cand = &h->slave_list[i];
        if (cand->valid && cand->telegram.u8id == slave_id)
        {
            s = cand;
            break;
        }
    }

    if (s == NULL)
    {
        osMutexRelease(h->xSlaveCfgMutex);
        return -1;
    }

    int valid_cnt = 0;
    for (uint8_t i = 0; i < MB_POINTS_PER_SLAVE; i++)
    {
        out[i] = s->points[i];
        if (out[i].valid) valid_cnt++;
    }

    osMutexRelease(h->xSlaveCfgMutex);
    return valid_cnt;
}

/* ========= 解析一个从站的所有有效点位 ========= */
int mb_pointmap_parse_slave(uint8_t slave_id, mb_parsed_point_t *out)
{
    if (out == NULL) return 0;

    mb_handle_t *h = mb_handle_get();
    if (h == NULL) return 0;

    osMutexAcquire(h->xSlaveCfgMutex, osWaitForever);

    mb_slave_runtime_t *s = NULL;
    for (uint8_t i = 0; i < h->slave_max_num; i++)
    {
        mb_slave_runtime_t *cand = &h->slave_list[i];
        if (cand->valid && cand->telegram.u8id == slave_id)
        {
            s = cand;
            break;
        }
    }

    if (s == NULL)
    {
        osMutexRelease(h->xSlaveCfgMutex);
        return 0;
    }

    // 数据锁保护 reg_data_buf + valid_point
    osMutexRelease(h->xSlaveCfgMutex);
    osMutexAcquire(h->xSlaveDataMutex, osWaitForever);

    int parsed_cnt = 0;
    uint16_t reg_len = s->valid_point;
    const uint16_t *reg = s->reg_data_buf;

    for (uint8_t i = 0; i < MB_POINTS_PER_SLAVE; i++)
    {
        const mb_point_t *pt = &s->points[i];
        if (!pt->valid) continue;
        if (pt->data_type >= MB_DT_MAX) continue;

        double raw = mb_pointmap_raw_value(reg, reg_len, pt->reg_index,
                                           pt->data_type, pt->bit_index);
        float physical = (float)(raw * (double)pt->scale + (double)pt->offset);

        out[parsed_cnt].value     = physical;
        out[parsed_cnt].raw_type  = pt->data_type;
        out[parsed_cnt].valid     = 1;
        memcpy(out[parsed_cnt].name, pt->name, MB_POINT_NAME_MAX);
        memcpy(out[parsed_cnt].unit, pt->unit, MB_POINT_UNIT_MAX);

        parsed_cnt++;
    }

    osMutexRelease(h->xSlaveDataMutex);
    return parsed_cnt;
}

/* ========= 全局上报模式 ========= */
int mb_pointmap_set_report_mode(mb_report_mode_t mode)
{
    if (!mb_runtime_config_begin()) return MB_UPGRADE_BUSY_ERR;
    s_report_mode = mode;
    mb_fire_cfg_change(MB_CFG_CHANGE_GLOBAL, 0);   /* 全局配置变更，slave_id=0 */
    mb_runtime_config_end();
    return MB_STATUS_OK;
}

mb_report_mode_t mb_pointmap_get_report_mode(void)
{
    return s_report_mode;
}

/** @brief 更新点位表与配置通知作为一个受门控保护的完整操作。 */
int mb_pointmap_set_slave_points(uint8_t slave_id, const mb_point_t *points)
{
    if (!mb_runtime_config_begin()) return MB_UPGRADE_BUSY_ERR;
    int result = mb_pointmap_set_slave_points_impl(slave_id, points);
    mb_runtime_config_end();
    return result;
}
