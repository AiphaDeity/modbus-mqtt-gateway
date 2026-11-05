/**
 * @file mb_pointmap.c
 * @brief Modbus 点位表配置实现 —— 寄存器值 → 物理量
 *
 *  核心换算：physical = raw * scale + offset
 *
 *  多字节寄存器拼接严格大端序：高寄存器在前（reg[i]<<16 | reg[i+1]）
 *  float32 通过 memcpy 走，规避严格别名规则。
 *
 *  本文件依赖 modbus_app.h（访问 slave_list / 锁 / cfg_change_cb）。
 */

#include "mb_pointmap.h"
#include "modbus_app.h"
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
 * @return 换算前的原始值（int64_t 中间态，大类型能装下）
 */
static double mb_pointmap_raw_value(const uint16_t *reg, uint16_t reg_len,
                                    uint16_t idx, mb_data_type_t type)
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
int mb_pointmap_set_slave_points(uint8_t slave_id, const mb_point_t *points)
{
    if (points == NULL)
    {
        return MB_BUILD_REQUEST_ERR;
    }

    mb_handle_t *h = mb_handle_get();
    if (h == NULL)
    {
        return MB_HANDLE_STATUS_ERR;
    }

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
        return MB_SLAVE_NOT_FOUND_ERR;
    }

    // 拷贝点位表到从站（这里假设 mb_slave_runtime_t 已挂 mb_point_t points[] 成员）
    // 编译时若该成员不存在会直接报错，提示去 modbus_app.h 加
    memcpy(&s->points, points, sizeof(s->points));

    osMutexRelease(h->xSlaveCfgMutex);

    // 触发存储方持久化
    if (h->cfg_change_cb)
    {
        h->cfg_change_cb(MB_CFG_CHANGE_UPDATE, slave_id, h->cfg_change_ctx);
    }

    return MB_STATUS_OK;
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

        double raw = mb_pointmap_raw_value(reg, reg_len, pt->reg_index, pt->data_type);
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
void mb_pointmap_set_report_mode(mb_report_mode_t mode)
{
    s_report_mode = mode;

    mb_handle_t *h = mb_handle_get();
    if (h != NULL && h->cfg_change_cb)
    {
        h->cfg_change_cb(MB_CFG_CHANGE_UPDATE, 0, h->cfg_change_ctx);
    }
}

mb_report_mode_t mb_pointmap_get_report_mode(void)
{
    return s_report_mode;
}
