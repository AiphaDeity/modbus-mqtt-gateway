/**
 * @file mb_slave.c
 * @brief Modbus 句柄管理 + 从站 CRUD + 配置回调
 *
 *  唯一负责 malloc/free 的模块。
 *  所有外部对从站列表的增删改都走这里，内部统一持有 xSlaveCfgMutex。
 */

#include "mb_internal.h"
#include "mb_pointmap.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========= 内部函数 ========= */

/**
 * @brief 触发配置变更回调（内部统一入口）
 *
 *  cb_suppress != 0 时静默（mb_apply_slave_config 合并多次变更为一次回调）。
 */
void mb_fire_cfg_change(mb_cfg_change_reason_t reason, uint8_t slave_id)
{
    if (mb_handle == NULL) return;
    if (mb_handle->cb_suppress) return;
    if (mb_handle->cfg_change_cb)
        mb_handle->cfg_change_cb(reason, slave_id, mb_handle->cfg_change_ctx);
}

/* ========= 对外 API ========= */

mb_handle_t *mb_handle_get(void)
{
    return mb_handle;   // 在 mb_poll.c 里定义，mb_internal.h extern
}

/**
 * @brief 初始化句柄本体 + 从站列表 + FreeRTOS 资源
 */
mb_status_e mb_handle_init(void)
{
    mb_handle = malloc(sizeof(mb_handle_t));
    if (mb_handle == NULL) return MB_INIT_STATUS_ERR;
    memset(mb_handle, 0, sizeof(mb_handle_t));

    mb_handle->slave_max_num   = MB_SLAVE_INIT_CAP;
    mb_handle->slave_actual_num = 0;
    mb_handle->slave_list = malloc(sizeof(mb_slave_runtime_t) * mb_handle->slave_max_num);
    if (mb_handle->slave_list == NULL)
    {
        free(mb_handle);
        return MB_INIT_STATUS_ERR;
    }
    memset(mb_handle->slave_list, 0, sizeof(mb_slave_runtime_t) * mb_handle->slave_max_num);

    mb_handle->xSlaveDataMutex = osMutexNew(NULL);
    if (mb_handle->xSlaveDataMutex == NULL) goto fail;

    mb_handle->xSlaveCfgMutex = osMutexNew(NULL);
    if (mb_handle->xSlaveCfgMutex == NULL)
    {
        osMutexDelete(mb_handle->xSlaveDataMutex);    /* 已创建的必须回收 */
        goto fail;
    }

    mb_handle->xWriteReqQueue = osMessageQueueNew(MB_WRITE_QUEUE_DEPTH,
                                                  sizeof(mb_write_request_t), NULL);
    if (mb_handle->xWriteReqQueue == NULL)
    {
        osMutexDelete(mb_handle->xSlaveCfgMutex);    /* 回收已创建的 mutex */
        osMutexDelete(mb_handle->xSlaveDataMutex);
        goto fail;
    }

    return MB_STATUS_OK;

fail:
    free(mb_handle->slave_list);
    free(mb_handle);
    mb_handle = NULL;
    return MB_INIT_STATUS_ERR;
}

/**
 * @brief 按 slave_id 查从站（只做 ID 匹配 + valid 判断，不加锁）
 *
 *  调用方必须自己持 xSlaveCfgMutex 定位 + 释放再操作。
 */
mb_slave_runtime_t *mb_get_slave_runtime(uint8_t slave_id)
{
    for (uint8_t i = 0; i < mb_handle->slave_max_num; i++)
    {
        mb_slave_runtime_t *s = &mb_handle->slave_list[i];
        if (s->valid && s->telegram.u8id == slave_id) return s;
    }
    return NULL;
}

void mb_set_config_change_cb(mb_cfg_change_cb_t cb, void *ctx)
{
    if (mb_handle == NULL) return;
    mb_handle->cfg_change_cb  = cb;
    mb_handle->cfg_change_ctx = ctx;
}

mb_status_e mb_realloc_slave_list_nolock(void)
{
    if (mb_handle->slave_max_num >= MB_SLAVE_HARD_MAX)
        return MB_REALLOC_STATUS_ERR;

    uint8_t new_cap = mb_handle->slave_max_num + MB_SLAVE_INIT_CAP;
    mb_slave_runtime_t *new_list = realloc(mb_handle->slave_list,
                                           sizeof(mb_slave_runtime_t) * new_cap);
    if (new_list == NULL) return MB_REALLOC_STATUS_ERR;   // 旧的还在

    // 新扩容的槽位清 0（realloc 不会清新增区域）
    memset(new_list + mb_handle->slave_max_num, 0,
           sizeof(mb_slave_runtime_t) * MB_SLAVE_INIT_CAP);

    mb_handle->slave_list  = new_list;
    mb_handle->slave_max_num = new_cap;
    return MB_STATUS_OK;
}

mb_status_e mb_realloc_slave_list(void)
{
    if (mb_handle == NULL) return MB_HANDLE_STATUS_ERR;
    osMutexAcquire(mb_handle->xSlaveCfgMutex, osWaitForever);
    mb_status_e ret = mb_realloc_slave_list_nolock();
    osMutexRelease(mb_handle->xSlaveCfgMutex);
    return ret;
}

/**
 * @brief 添加从站核心逻辑（不加锁，调用方必须已持 xSlaveCfgMutex）
 */
mb_status_e mb_add_slave_nolock(modbus_t *cfg, uint8_t *existed)
{
    if (existed != NULL)
        *existed = (mb_get_slave_runtime(cfg->u8id) != NULL) ? 1 : 0;

    // 已存在 → UPDATE
    mb_slave_runtime_t *slave = mb_get_slave_runtime(cfg->u8id);
    if (slave != NULL)
    {
        memcpy(&slave->telegram, cfg, sizeof(modbus_t));
        slave->telegram.u16reg = slave->reg_data_buf;
        slave->telegram.u32CurrentTask = NULL;
        slave->valid = 1;
        return MB_STATUS_OK;
    }

    // 容量不够 → 扩容
    if (mb_handle->slave_actual_num >= mb_handle->slave_max_num)
    {
        mb_status_e r = mb_realloc_slave_list_nolock();
        if (r != MB_STATUS_OK) return r;
    }

    // 找空位 INSERT
    for (uint8_t i = 0; i < mb_handle->slave_max_num; i++)
    {
        if (mb_handle->slave_list[i].valid == 0)
        {
            slave = &mb_handle->slave_list[i];
            break;
        }
    }

    memcpy(&slave->telegram, cfg, sizeof(modbus_t));
    slave->telegram.u16reg = slave->reg_data_buf;
    slave->telegram.u32CurrentTask = NULL;
    slave->valid = 1;
    mb_handle->slave_actual_num++;

    MB_LOGI("slave added: id=%d, FC=%d, addr=%u, count=%u",
            slave->telegram.u8id, slave->telegram.u8fct,
            slave->telegram.u16RegAdd, slave->telegram.u16CoilsNo);

    return MB_STATUS_OK;
}

/**
 * @brief 添加从站（公开加锁版）
 */
mb_status_e mb_add_slave(modbus_t *cfg)
{
    if (mb_handle == NULL) return MB_HANDLE_STATUS_ERR;
    if (cfg == NULL) return MB_BUILD_REQUEST_ERR;

    // 参数校验（无竞争，不加锁）
    if (cfg->u8fct < MB_FC_READ_COILS ||
        cfg->u8fct > MB_FC_READ_INPUT_REGISTER)
    {
        MB_LOGE("reject slave %d: u8fct=0x%02X not supported (only FC01~FC04)",
                cfg->u8id, cfg->u8fct);
        return MB_NOT_SUPPORT_FC_ERR;
    }

    uint16_t max_qty =
        (cfg->u8fct == MB_FC_READ_COILS || cfg->u8fct == MB_FC_READ_DISCRETE_INPUT)
        ? (MB_REG_MAX_PER_SLAVE * 16U)   // 512 个线圈
        : MB_REG_MAX_PER_SLAVE;          // 32 个寄存器
    if (cfg->u16CoilsNo > max_qty)
    {
        MB_LOGE("reject slave %d: u16CoilsNo=%u > %u (fc=0x%02X)",
                cfg->u8id, cfg->u16CoilsNo, max_qty, cfg->u8fct);
        return MB_BUILD_REQUEST_ERR;
    }

    // 持 cfg 锁保护 + 调核心逻辑
    osMutexAcquire(mb_handle->xSlaveCfgMutex, osWaitForever);
    uint8_t existed = 0;
    mb_status_e ret = mb_add_slave_nolock(cfg, &existed);
    osMutexRelease(mb_handle->xSlaveCfgMutex);

    // 回调在锁外触发（避免回调里再请求 cfg 锁死锁）
    if (ret == MB_STATUS_OK)
    {
        mb_fire_cfg_change(existed ? MB_CFG_CHANGE_UPDATE : MB_CFG_CHANGE_ADD,
                           cfg->u8id);
    }

    return ret;
}

/**
 * @brief 删除从站核心逻辑（不加锁，调用方必须已持 xSlaveCfgMutex）
 */
mb_status_e mb_delete_slave_nolock(uint8_t slave_id)
{
    mb_slave_runtime_t *slave = mb_get_slave_runtime(slave_id);
    if (slave == NULL) return MB_SLAVE_NOT_FOUND_ERR;

    slave->valid = 0;
    mb_handle->slave_actual_num--;
    MB_LOGI("slave deleted: id=%d", slave_id);

    return MB_STATUS_OK;
}

/**
 * @brief 删除从站（公开加锁版）
 */
mb_status_e mb_delete_slave(uint8_t slave_id)
{
    if (mb_handle == NULL) return MB_HANDLE_STATUS_ERR;

    osMutexAcquire(mb_handle->xSlaveCfgMutex, osWaitForever);
    mb_status_e ret = mb_delete_slave_nolock(slave_id);
    osMutexRelease(mb_handle->xSlaveCfgMutex);

    if (ret == MB_STATUS_OK)
        mb_fire_cfg_change(MB_CFG_CHANGE_DELETE, slave_id);

    return ret;
}

void mb_notify_reload_complete(void)
{
    if (mb_handle == NULL) return;
    MB_LOGI("slave reload complete: %u slaves", mb_handle->slave_actual_num);
    mb_fire_cfg_change(MB_CFG_CHANGE_GLOBAL, 0);   /* 上电加载属于全局变更 */
}

void mb_config_batch_begin(void)
{
    if (mb_handle == NULL) return;
    mb_handle->cb_suppress = 1;
}

void mb_config_batch_end(void)
{
    if (mb_handle == NULL) return;
    mb_handle->cb_suppress = 0;
    MB_LOGI("config batch end: %u slaves", mb_handle->slave_actual_num);
    mb_fire_cfg_change(MB_CFG_CHANGE_GLOBAL, 0);   /* 批量结束属于全局变更 */
}
