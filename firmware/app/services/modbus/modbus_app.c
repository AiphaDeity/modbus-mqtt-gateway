#include "modbus_app.h"
#include <stdlib.h>
#include <string.h>
#include "modbus_proto.h"
#include "modbus.h"

mb_handle_t *mb_handle = NULL;

/*----------------------------------------------*/
/**
 * @brief 根据连续失败次数得到本次冷却时长，带封顶
 */
static uint32_t mb_get_backoff_cooling_ms(uint8_t consecutive_fail)
{
    if (consecutive_fail == 1)
    {
        return 5000;
    }
    else if (consecutive_fail == 2)
    {
        return 10000;
    }
    else
    {
        // >=3 之后封顶 20s，不再放大
        return 20000;
    }
}


static void mb_poll_idle()
{
    mb_slave_runtime_t *slave = NULL;
    uint32_t now = osKernelGetTickCount();
    // 寻找符合采集条件的从站
    for (uint8_t i = 0; i < mb_handle->slave_max_num; i++)
    {
        slave = &mb_handle->slave_list[i];

        if (slave->valid == 0 || slave->cfg.enable == 0) continue;
        // ========== 阶梯冷却判断 ==========
        if (slave->consecutive_fail > 0)
        {
            uint32_t cooling = mb_get_backoff_cooling_ms(slave->consecutive_fail);
            if ((now - slave->last_trigger_tick) < cooling)
            {
                continue; // 还在冷却窗口内，跳过
            }
        }
        // 正常采集周期判断
        if ((now - slave->last_trigger_tick) < slave->cfg.collect_interval_ms) continue;

        // 选中，发起新一轮采集
        mb_handle->waiting_slave = slave;
        slave->last_trigger_tick = now;
        slave->fail_count = 0;
        mb_handle->state = MB_STATE_TX;
        break;
    }
}

static void mb_poll_tx()
{
    mb_slave_runtime_t *slave = mb_handle->waiting_slave;
    if (slave == NULL)
    {
        mb_handle->state = MB_STATE_IDLE;
        return;
    }
    mb_status_e ret = mb_send_slave_request(slave);
    if (ret == MB_STATUS_OK)
    {
        // DMA提交成功，等待 TxCplt 回调切 WAIT_ACK
    }
    else
    {
        mb_handle->state = MB_STATE_IDLE;
        mb_handle->waiting_slave = NULL;
    }
}



static void mb_poll_wait_ack()
{
    uint32_t now = osKernelGetTickCount();
    mb_slave_runtime_t *slave = mb_handle->waiting_slave;
    if (slave == NULL)
    {
        mb_handle->state = MB_STATE_IDLE;
        return;
    }

    if ((now - mb_handle->send_tick) >= mb_handle->timeout_ms)
    {
        slave->collect_status = 2;
        slave->is_online = 0;
        slave->fail_count++;

        if (slave->fail_count <= slave->cfg.retry_cnt)
        {
            // 本轮继续重试
            mb_handle->state = MB_STATE_TX;
        }
        else
        {
            // ✅ 本轮所有重试耗尽：连续失败计数 +1
            if (slave->consecutive_fail < 255)
            {
                slave->consecutive_fail++;
            }
            mb_handle->state = MB_STATE_IDLE;
            mb_handle->waiting_slave = NULL;
        }
    }
}



static void mb_poll_err()
{
    mb_handle->state = MB_STATE_IDLE;
    mb_handle->waiting_slave = NULL;
}


/*----------------------------------------------*/


/**
 * @brief 初始化从站句柄
 * 
 * @return mb_status_e 初始化状态
 */
mb_status_e mb_handle_init(void)
{
    // 1. 分配句柄本体
    mb_handle = malloc(sizeof(mb_handle_t));
    if (mb_handle == NULL)
    {
        return MB_INIT_STATUS_ERR;
    }
    memset(mb_handle, 0, sizeof(mb_handle_t));

    // 2. 初始化从站列表，初始容量 4
    mb_handle->slave_max_num = MB_SLAVE_INIT_CAP;
    mb_handle->slave_actual_num = 0;
    mb_handle->slave_list = malloc(sizeof(mb_slave_runtime_t) * mb_handle->slave_max_num);
    if(mb_handle->slave_list == NULL)
    {
        free(mb_handle);
        return MB_INIT_STATUS_ERR;
    }
    memset(mb_handle->slave_list, 0, sizeof(mb_slave_runtime_t) * mb_handle->slave_max_num);

    // 3. 帧池默认已经清零，valid=0 空闲
    // 4. dma_rx_buf、tx_busy 已经 memset 清零

    // 二值信号量：初始0，中断释放，任务等待
    mb_handle->xNewFrameNotifySem = osSemaphoreNew(1, 0, NULL);
    if (mb_handle->xNewFrameNotifySem == NULL)
    {
        free(mb_handle->slave_list);
        free(mb_handle);
        return MB_INIT_STATUS_ERR;
    }

    // 互斥锁，默认属性 NULL
    mb_handle->xSlaveDataMutex = osMutexNew(NULL);
    if (mb_handle->xSlaveDataMutex == NULL)
    {
        free(mb_handle->slave_list);
        free(mb_handle);
        return MB_INIT_STATUS_ERR;
    }

    // 互斥锁：从站静态配置（slave_id、地址、间隔、使能，低频改）
    mb_handle->xSlaveCfgMutex = osMutexNew(NULL);
    if (mb_handle->xSlaveCfgMutex == NULL)
    {
        free(mb_handle->slave_list);
        free(mb_handle);
        return MB_INIT_STATUS_ERR;
    }

    mb_handle->poll_task_handle = NULL;    // 轮询任务句柄

    return MB_STATUS_OK;

}

/**
 * @brief 添加从站
 * 
 * @param cfg 从站配置指针
 * @return mb_status_e 添加状态
 */
mb_status_e mb_add_slave(mb_slave_cfg_t *cfg)
{
    if (mb_handle == NULL)
    {
        return MB_HANDLE_STATUS_ERR;
    }

    // 检查从站ID是否已存在
    mb_slave_runtime_t *slave = mb_get_slave_runtime(cfg->slave_id);
    if (slave != NULL)
    {
        // 更新配置
        slave->cfg.reg_start_addr = cfg->reg_start_addr;
        slave->cfg.reg_cnt = cfg->reg_cnt;
        slave->cfg.enable = cfg->enable;
        return MB_STATUS_OK;
    }
    
    if (mb_handle->slave_actual_num >= mb_handle->slave_max_num)
    {
        if(mb_realloc_slave_list() != MB_STATUS_OK)
        {
            return MB_REALLOC_STATUS_ERR;
        }
    }

    //找一个空闲的从站位置
    for (uint8_t i = 0; i < mb_handle->slave_max_num; i++)
    {
        if (mb_handle->slave_list[i].valid == 0)
        {
            slave = &mb_handle->slave_list[i];
            break;
        }
    }

    // 初始化从站配置
    slave->cfg.slave_id = cfg->slave_id;
    slave->cfg.reg_start_addr = cfg->reg_start_addr;
    slave->cfg.reg_cnt = cfg->reg_cnt;
    slave->cfg.enable = cfg->enable;
    slave->valid = 1;
    mb_handle->slave_actual_num++;

    // 初始化预编译好的请求帧
    if(mb_build_slave_request(slave) != MB_STATUS_OK)
    {
        return MB_BUILD_REQUEST_ERR;
    }

    return MB_STATUS_OK;
}

/*删除从站
*/
/**
 * @brief 删除从站
 * 
 * @param slave_id 从站ID
 * @return mb_status_e 删除状态
 */
mb_status_e mb_delete_slave(uint8_t slave_id)
{
    mb_slave_runtime_t *slave = mb_get_slave_runtime(slave_id);
    if (slave == NULL)
    {
        return MB_SLAVE_NOT_FOUND_ERR;
    }

    // 标记为无效
    slave->valid = 0;
    mb_handle->slave_actual_num--;

    return MB_STATUS_OK;
}


/**
 * @brief 扩容从站列表
 * 
 * @return mb_status_e 扩容状态
 */
mb_status_e mb_realloc_slave_list(void)
{
    if(mb_handle->slave_max_num >= MB_SLAVE_HARD_MAX)
    {
        return MB_REALLOC_STATUS_ERR;
    }
    mb_handle->slave_list = realloc(mb_handle->slave_list, mb_handle->slave_max_num + MB_SLAVE_INIT_CAP);
    if(mb_handle->slave_list == NULL)
    {
        return MB_REALLOC_STATUS_ERR;
    }
    mb_handle->slave_max_num += MB_SLAVE_INIT_CAP;

    return MB_STATUS_OK;
}

/**
 * @brief 获取从站运行时数据
 * 
 * @param slave_id 从站ID
 * @return mb_slave_runtime_t* 从站运行时数据指针，若不存在则返回 NULL
 */
mb_slave_runtime_t* mb_get_slave_runtime(uint8_t slave_id)
{
    for (uint8_t i = 0; i < mb_handle->slave_max_num; i++)
    {
        if (mb_handle->slave_list[i].cfg.slave_id == slave_id  && mb_handle->slave_list[i].valid == 1)
        {
            return &mb_handle->slave_list[i];
        }
    }
    return NULL;
}


/**
 * @brief 数据入槽
 * 
 * @param len 数据长度
 * @return mb_status_e 入槽状态
 */
mb_status_e mb_in_slot(uint16_t len)
{
    memcpy(mb_handle->frame_slot.data, mb_handle->dma_rx_buf, len);
    mb_handle->frame_slot.frame_len = len;
    mb_handle->frame_slot.recv_tick = osKernelGetTickCount();
    mb_handle->frame_slot.valid = 1;
    return MB_STATUS_OK; 
}


/**
 * @brief 轮询从站状态机推进函数，在poll任务循环持续调用
 * @note 接收应答逻辑在 mb_parse_frame，由信号量唤醒，不在此处
 */
mb_status_e mb_poll_slave(void)
{
    switch (mb_handle->state)
    {
        case MB_STATE_IDLE:
        {
            mb_poll_idle();
            break;
        }

        case MB_STATE_TX:
        {
            mb_poll_tx();
            break;
        }

        case MB_STATE_WAIT_ACK:
        {
            mb_poll_wait_ack();
            break;
        }

        case MB_STATE_ERR:
        {
            mb_poll_err();
            break;
        }
        default:
        {
            mb_handle->state = MB_STATE_IDLE;
            mb_handle->waiting_slave = NULL;
            break;
        }
    }
    return MB_STATUS_OK;
}





/**
 * @brief 发送从站请求帧
 * 
 * @param slave 从站运行时数据指针
 * @return mb_status_e 发送状态
 */
mb_status_e mb_send_slave_request(mb_slave_runtime_t *slave)
{
    if (HAL_UART_Transmit_DMA(&MODBUS_RS485_UART, slave->tx_req_buf, slave->tx_req_len) != HAL_OK)
    {
        return MB_SEND_REQUEST_ERR;
    }
    // 回调里面： mb_handle->state = MB_STATE_WAIT_ACK; mb_handle->send_tick = now; 

    return MB_STATUS_OK;
}







