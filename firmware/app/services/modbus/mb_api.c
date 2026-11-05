/**
 * @file mb_api.c
 * @brief Modbus 外部接口（MQTT / 存储方 pull 模式 API）
 *
 *  所有接口返回拷贝值，返回后调用者可自由使用，与 poll 任务完全解耦。
 *  内部按用途持锁，绝不同时持两把锁（避免死锁）。
 */

#include "mb_internal.h"
#include "mb_pointmap.h"
#include <string.h>
#include <stdio.h>

/* ========= 外部写请求 ========= */

/**
 * @brief 外部注入写请求（FC6 / FC16）
 *
 *  MQTT / 配置管理任务调用，请求投入队列后 poll 任务在 IDLE 优先取出。
 *  写入值直接拷贝，调用者传完即可释放。
 */
mb_status_e mb_inject_write(uint8_t slave_id,
                            uint8_t func_code,
                            uint16_t reg_addr,
                            uint16_t reg_count,
                            const uint16_t *write_values,
                            osSemaphoreId_t result_sem,
                            mb_errot_t *result_err_out)
{
    if (mb_handle == NULL)              return MB_HANDLE_STATUS_ERR;
    if (func_code != MB_FC_WRITE_REGISTER && func_code != MB_FC_WRITE_MULTIPLE_REGISTERS)
                                        return MB_NOT_SUPPORT_FC_ERR;
    if (write_values == NULL)           return MB_BUILD_REQUEST_ERR;
    if (reg_count > MB_REG_MAX_PER_SLAVE)
                                        return MB_BUILD_REQUEST_ERR;

    mb_write_request_t wr;
    memset(&wr, 0, sizeof(wr));
    wr.slave_id       = slave_id;
    wr.func_code      = func_code;
    wr.reg_addr       = reg_addr;
    wr.reg_count      = reg_count;
    wr.result_sem     = result_sem;
    wr.result_err_out = result_err_out;
    for (uint16_t i = 0; i < reg_count; i++)
        wr.write_values[i] = write_values[i];

    if (osMessageQueuePut(mb_handle->xWriteReqQueue, &wr, 0, 0) != osOK)
    {
        MB_LOGW("write req queue full, slave=%d", slave_id);
        return MB_QUEUE_FULL_ERR;
    }

    // 同步等待：poll 任务处理完后 release + 填 *result_err_out
    if (result_sem != NULL)
        osSemaphoreAcquire(result_sem, osWaitForever);

    return MB_STATUS_OK;
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
