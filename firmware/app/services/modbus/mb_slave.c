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
    if (mb_handle->xSlaveCfgMutex == NULL) goto fail;

    mb_handle->xWriteReqQueue = osMessageQueueNew(MB_WRITE_QUEUE_DEPTH,
                                                  sizeof(mb_write_request_t), NULL);
    if (mb_handle->xWriteReqQueue == NULL) goto fail;

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

mb_status_e mb_realloc_slave_list(void)
{
    if (mb_handle->slave_max_num >= MB_SLAVE_HARD_MAX)
        return MB_REALLOC_STATUS_ERR;

    uint8_t new_cap = mb_handle->slave_max_num + MB_SLAVE_INIT_CAP;
    mb_slave_runtime_t *new_list = realloc(mb_handle->slave_list,
                                           sizeof(mb_slave_runtime_t) * new_cap);
    if (new_list == NULL) return MB_REALLOC_STATUS_ERR;   // 旧的还在

    mb_handle->slave_list  = new_list;
    mb_handle->slave_max_num = new_cap;
    return MB_STATUS_OK;
}

/**
 * @brief 添加从站（已存在则 UPDATE，不存在则 INSERT）
 */
mb_status_e mb_add_slave(modbus_t *cfg)
{
    if (mb_handle == NULL) return MB_HANDLE_STATUS_ERR;
    if (cfg->u16CoilsNo > MB_REG_MAX_PER_SLAVE)
    {
        MB_LOGE("reject slave %d: u16CoilsNo=%u > %u",
                cfg->u8id, cfg->u16CoilsNo, MB_REG_MAX_PER_SLAVE);
        return MB_BUILD_REQUEST_ERR;
    }

    // 已存在 → UPDATE
    mb_slave_runtime_t *slave = mb_get_slave_runtime(cfg->u8id);
    if (slave != NULL)
    {
        memcpy(&slave->telegram, cfg, sizeof(modbus_t));
        slave->telegram.u16reg = slave->reg_data_buf;
        slave->telegram.u32CurrentTask = NULL;
        slave->valid = 1;

        if (mb_handle->cfg_change_cb)
            mb_handle->cfg_change_cb(MB_CFG_CHANGE_UPDATE, slave->telegram.u8id,
                                     mb_handle->cfg_change_ctx);
        return MB_STATUS_OK;
    }

    // 容量不够 → 扩容
    if (mb_handle->slave_actual_num >= mb_handle->slave_max_num)
    {
        if (mb_realloc_slave_list() != MB_STATUS_OK) return MB_REALLOC_STATUS_ERR;
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

    if (mb_handle->cfg_change_cb)
        mb_handle->cfg_change_cb(MB_CFG_CHANGE_ADD, slave->telegram.u8id,
                                 mb_handle->cfg_change_ctx);

    return MB_STATUS_OK;
}

mb_status_e mb_delete_slave(uint8_t slave_id)
{
    mb_slave_runtime_t *slave = mb_get_slave_runtime(slave_id);
    if (slave == NULL) return MB_SLAVE_NOT_FOUND_ERR;

    slave->valid = 0;
    mb_handle->slave_actual_num--;
    MB_LOGI("slave deleted: id=%d", slave_id);

    if (mb_handle->cfg_change_cb)
        mb_handle->cfg_change_cb(MB_CFG_CHANGE_DELETE, slave_id, mb_handle->cfg_change_ctx);

    return MB_STATUS_OK;
}

void mb_notify_reload_complete(void)
{
    if (mb_handle == NULL) return;
    MB_LOGI("slave reload complete: %u slaves", mb_handle->slave_actual_num);
    if (mb_handle->cfg_change_cb)
        mb_handle->cfg_change_cb(MB_CFG_CHANGE_RELOAD, 0, mb_handle->cfg_change_ctx);
}
