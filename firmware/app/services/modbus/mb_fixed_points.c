/**
 * @file mb_fixed_points.c
 * @brief 编译期固定只读点表 —— 启动时应用（最小配置首次启用入口）
 *
 *  此函数在 Poll 任务启动时调用，替代旧的 test_add_three_slaves()。
 *  不依赖 Flash 配置或 JSON 解析器，直接用编译期常量注册从站和点位。
 *  后续接入 storage_config 持久化时，此函数作为"无 Flash 配置时的降级默认"。
 */
#include "mb_fixed_points.h"
#include "mb_pointmap.h"
#include <string.h>

/* ===== 单站配置模板 ===== */

/* 站1: 光照传感器 B-RS-L30 */
static const modbus_t s_cfg_light = {
    .u8id               = MB_FIXED_ADDR_LIGHT,
    .u8fct              = MB_FC_READ_REGISTERS,       /* FC03 */
    .u16RegAdd          = MB_FIXED_REG_LIGHT,          /* 0x0002 */
    .u16CoilsNo         = MB_FIXED_REG_LIGHT_CNT,     /* 2 寄存器 */
    .u16reg             = NULL,                        /* 运行期内部用 reg_data_buf */
    .u32CurrentTask     = NULL,
    .collect_interval_ms = MB_FIXED_POLL_MS_LIGHT,    /* 2000ms */
    .enable             = 1,
    .retry_cnt          = 1,
};

/* 站2~4: 水浸传感器（共用模板，地址不同） */
static const modbus_t s_cfg_leak = {
    .u8fct              = MB_FC_READ_REGISTERS,       /* FC03 */
    .u16RegAdd          = MB_FIXED_REG_LEAK,           /* 0x2380 */
    .u16CoilsNo         = MB_FIXED_REG_LEAK_CNT,      /* 1 寄存器 */
    .u16reg             = NULL,
    .u32CurrentTask     = NULL,
    .collect_interval_ms = MB_FIXED_POLL_MS_LEAK,     /* 1000ms */
    .enable             = 1,
    .retry_cnt          = 2,                           /* 报警类多重试一次 */
};

/* ===== 点位表 ===== */

/* 光照传感器: 1 个点（UINT32，÷1000=Lux） */
static mb_point_t s_pts_light[MB_POINTS_PER_SLAVE] = {
    [0] = {
        .reg_index  = 0,                  /* reg_data_buf[0..1] 拼成 32 位 */
        .data_type  = MB_DT_UINT32,       /* 高 16 位在前（Modbus 大端） */
        .scale      = 0.001f,             /* raw / 1000 = Lux */
        .offset     = 0.0f,
        .name       = "light",            /* ≤7 字符 */
        .unit       = "Lux",              /* ≤7 字符 */
        .valid      = 1,
        .bit_index  = 0,
    },
    /* [1..15] valid=0（空位，memset 已清零） */
};

/* 水浸传感器: 1 个点（UINT16，0=无水 1=有水） */
static mb_point_t s_pts_leak[MB_POINTS_PER_SLAVE] = {
    [0] = {
        .reg_index  = 0,
        .data_type  = MB_DT_UINT16,
        .scale      = 1.0f,               /* 原始值直接用 */
        .offset     = 0.0f,
        .name       = "water",
        .unit       = "",                 /* 无单位（状态量） */
        .valid      = 1,
        .bit_index  = 0,
    },
};

/* ===== 应用函数 ===== */

bool mb_fixed_points_apply(void)
{
    /* 幂等：先清空已有从站（重复调用安全） */
    for (uint8_t id = 1; id <= MB_FIXED_SLAVE_COUNT; id++)
    {
        mb_apply_slave_config(MB_CFG_OP_DELETE, id, NULL, NULL);
    }

    /* 批量添加：抑制中间回调，只触发一次 RELOAD */
    mb_config_batch_begin();

    mb_status_e st;

    /* 站1: 光照传感器 */
    st = mb_apply_slave_config(MB_CFG_OP_UPSERT,
                               MB_FIXED_ADDR_LIGHT,
                               &s_cfg_light, s_pts_light);
    if (st != MB_STATUS_OK) { mb_config_batch_end(); return false; }

    /* 站2~4: 水浸传感器（模板复制，改地址） */
    const uint8_t leak_ids[3] = {
        MB_FIXED_ADDR_LEAK_1,
        MB_FIXED_ADDR_LEAK_2,
        MB_FIXED_ADDR_LEAK_3,
    };
    for (int i = 0; i < 3; i++)
    {
        modbus_t cfg = s_cfg_leak;       /* 值拷贝模板 */
        cfg.u8id = leak_ids[i];
        st = mb_apply_slave_config(MB_CFG_OP_UPSERT,
                                   leak_ids[i],
                                   &cfg, s_pts_leak);
        if (st != MB_STATUS_OK) { mb_config_batch_end(); return false; }
    }

    mb_config_batch_end();
    return true;
}
