/**
 * @file mb_api.c
 * @brief Modbus 外部接口（MQTT / 存储方 pull 模式 API）
 *
 *  所有接口返回拷贝值，返回后调用者可自由使用，与 poll 任务完全解耦。
 *  内部按用途持锁，绝不同时持两把锁（避免死锁）。
 */

#include "mb_internal.h"
#include "mb_pointmap.h"
#include "upgrade_quiesce.h"
#include <string.h>
#include <stdio.h>

enum { MB_RESULT_WAITING = 1, MB_RESULT_DONE, MB_RESULT_ABANDONED };
typedef struct
{
    uint32_t id;
    uint8_t state;
    mb_errot_t error;
} mb_write_result_t;

static mb_write_result_t s_write_results[MB_WRITE_QUEUE_DEPTH + 1U];

/** @brief 已完成结果也须等同步调用方取走，不能只看请求队列为空。 */
bool mb_write_results_settled(void)
{
    taskENTER_CRITICAL();
    bool settled = true;
    for (uint32_t i = 0U; i < MB_WRITE_QUEUE_DEPTH + 1U; i++)
        settled = settled && s_write_results[i].id == 0U;
    taskEXIT_CRITICAL();
    return settled;
}

/** @brief 在途结果由模块持有，队列不保存调用者信号量或栈地址。 */
static mb_write_result_t *mb_result_find(uint32_t id)
{
    for (uint32_t i = 0U; i < MB_WRITE_QUEUE_DEPTH + 1U; i++)
    {
        if (s_write_results[i].id == id) return &s_write_results[i];
    }
    return NULL;
}

/** @brief 同步等待者已离开时，尚未发送的请求直接取消。 */
bool mb_write_pending(const mb_write_request_t *request)
{
    if (request->completion_id == 0U) return true;
    taskENTER_CRITICAL();
    mb_write_result_t *result = mb_result_find(request->completion_id);
    bool pending = result != NULL && result->state == MB_RESULT_WAITING;
    taskEXIT_CRITICAL();
    return pending;
}

/** @brief 完成或拒绝一条写请求；超时者的票据只回收，不访问其原始缓冲。 */
void mb_finish_write(const mb_write_request_t *request, mb_errot_t error)
{
    if (request->completion_id == 0U) return;
    taskENTER_CRITICAL();
    mb_write_result_t *result = mb_result_find(request->completion_id);
    if (result != NULL)
    {
        if (result->state == MB_RESULT_ABANDONED)
            memset(result, 0, sizeof(*result));
        else
        {
            result->error = error;
            result->state = MB_RESULT_DONE;
        }
    }
    taskEXIT_CRITICAL();
}

/* ========= 统一配置入口 ========= */

/**
 * @brief 统一配置入口（MQTT / Web 平台下发推荐用这一个）
 *
 *  内部自己持 cfg 锁 + 调 _nolock 版本 CRUD，避免与公开加锁版嵌套死锁。
 *  多次内部变更合并为一次 cfg_change_cb 回调（存储方只写一次 Flash）。
 */
static mb_status_e mb_apply_slave_config_impl(mb_cfg_op_t op,
                                  uint8_t slave_id,
                                  const modbus_t *cfg,
                                  const mb_point_t *points)
{
    if (mb_handle == NULL) return MB_HANDLE_STATUS_ERR;

    /* ---------- 删除 ---------- */
    if (op == MB_CFG_OP_DELETE)
    {
        return mb_delete_slave(slave_id);   /* 内部已加锁 */
    }

    /* ---------- 新增 / 更新 ---------- */
    if (op != MB_CFG_OP_UPSERT)          return MB_BUILD_REQUEST_ERR;
    if (cfg == NULL && points == NULL)   return MB_BUILD_REQUEST_ERR;  // 无事可做

    // 参数校验（无竞争，不加锁）
    if (cfg != NULL)
    {
        if (cfg->u8fct < MB_FC_READ_COILS ||
            cfg->u8fct > MB_FC_READ_INPUT_REGISTER)
            return MB_NOT_SUPPORT_FC_ERR;

        uint16_t max_qty =
            (cfg->u8fct == MB_FC_READ_COILS || cfg->u8fct == MB_FC_READ_DISCRETE_INPUT)
            ? (MB_REG_MAX_PER_SLAVE * 16U)   // 512 个线圈
            : MB_REG_MAX_PER_SLAVE;          // 32 个寄存器
        if (cfg->u16CoilsNo > max_qty)
            return MB_BUILD_REQUEST_ERR;
    }

    /* ---- 持一把 cfg 锁，内部全用 _nolock 版本 ---- */
    osMutexAcquire(mb_handle->xSlaveCfgMutex, osWaitForever);

    uint8_t existed = (mb_get_slave_runtime(slave_id) != NULL) ? 1 : 0;
    mb_handle->cb_suppress = 1;     /* 抑制内部 _nolock 的回调 */

    mb_status_e ret = MB_STATUS_OK;

    if (cfg != NULL)
    {
        modbus_t tmp = *cfg;
        tmp.u8id = slave_id;      /* 以防调用方 cfg->u8id 与 slave_id 入参不一致 */
        ret = mb_add_slave_nolock(&tmp, NULL);
    }

    if (ret == MB_STATUS_OK && points != NULL)
    {
        mb_slave_runtime_t *s = mb_get_slave_runtime(slave_id);
        if (s != NULL)
        {
            memcpy(&s->points, points, sizeof(s->points));   /* 持锁直接写 */
        }
        else
        {
            ret = MB_SLAVE_NOT_FOUND_ERR;   /* 只传 points 但从站不存在 */
        }
    }

    mb_handle->cb_suppress = 0;
    osMutexRelease(mb_handle->xSlaveCfgMutex);   /* 先释放 */

    /* ---- 回调在锁外触发（避免回调里再请求 cfg 锁死锁）---- */
    if (ret == MB_STATUS_OK)
    {
        mb_fire_cfg_change(existed ? MB_CFG_CHANGE_UPDATE : MB_CFG_CHANGE_ADD,
                           slave_id);
    }

    return ret;
}

/* ========= 外部写请求 ========= */

// 旧入口没有命令身份、时效及持久受理信息，只保留签名并明确拒绝。
mb_status_e mb_inject_write(uint8_t slave_id,
                            uint8_t func_code,
                            uint16_t reg_addr,
                            uint16_t reg_count,
                            const uint16_t *write_values,
                            osSemaphoreId_t result_sem,
                            mb_errot_t *result_err_out,
                            uint32_t result_timeout_ms)
{
    (void)slave_id;
    (void)func_code;
    (void)reg_addr;
    (void)reg_count;
    (void)write_values;
    (void)result_sem;
    (void)result_timeout_ms;
    if (result_err_out != NULL) *result_err_out = ERR_CONTROL_REJECTED;
    return MB_CONTROL_REJECTED_ERR;
}

/* ========= MQTT / 存储方 pull 接口 ========= */

uint8_t mb_for_each_valid_slave(mb_slave_iter_cb_t cb, void *ctx)
{
    if (mb_handle == NULL || cb == NULL) return 0;

    uint8_t count = 0;
    osMutexAcquire(mb_handle->xSlaveCfgMutex, osWaitForever);

    for (uint8_t i = 0; i < mb_handle->slave_max_num; i++)
    {
        mb_slave_runtime_t *s = &mb_handle->slave_list[i];
        if (!s->valid || !s->telegram.enable) continue;

        count++;
        if (cb(s, ctx) != 0) break;
    }

    osMutexRelease(mb_handle->xSlaveCfgMutex);
    return count;
}

mb_status_e mb_take_slave_snapshot(uint8_t slave_id, mb_slave_snapshot_t *snap_out)
{
    if (mb_handle == NULL || snap_out == NULL) return MB_HANDLE_STATUS_ERR;

    // 先用配置锁定位（避免遍历时刚好被 delete 标记无效）
    osMutexAcquire(mb_handle->xSlaveCfgMutex, osWaitForever);
    mb_slave_runtime_t *s = mb_get_slave_runtime(slave_id);
    if (s == NULL || !s->telegram.enable)
    {
        osMutexRelease(mb_handle->xSlaveCfgMutex);
        return MB_SLAVE_NOT_FOUND_ERR;
    }
    osMutexRelease(mb_handle->xSlaveCfgMutex);

    // 再拿数据锁拷贝寄存器（poll 任务写，这里读）
    osMutexAcquire(mb_handle->xSlaveDataMutex, osWaitForever);

    snap_out->slave_id   = s->telegram.u8id;
    snap_out->func_code  = s->telegram.u8fct;
    snap_out->is_online  = s->is_online;
    snap_out->data_valid = s->data_valid;
    snap_out->reg_addr   = s->telegram.u16RegAdd;
    snap_out->valid_point = s->valid_point;
    snap_out->last_collect_tick = s->last_trigger_tick;
    snap_out->collect_status    = s->collect_status;

    uint16_t copy_len = (s->valid_point > MB_REG_MAX_PER_SLAVE)
                        ? MB_REG_MAX_PER_SLAVE : s->valid_point;
    memcpy(snap_out->data, s->reg_data_buf, sizeof(uint16_t) * copy_len);

    osMutexRelease(mb_handle->xSlaveDataMutex);
    return MB_STATUS_OK;
}

/** @brief 完整配置调用及通知都受升级门控保护，暂停时拒绝新变更。 */
mb_status_e mb_apply_slave_config(mb_cfg_op_t op, uint8_t slave_id,
                                  const modbus_t *cfg, const mb_point_t *points)
{
    if (!mb_runtime_config_begin()) return MB_UPGRADE_BUSY_ERR;
    mb_status_e result = mb_apply_slave_config_impl(op, slave_id, cfg, points);
    mb_runtime_config_end();
    return result;
}
