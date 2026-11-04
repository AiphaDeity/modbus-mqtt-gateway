#include "modbus_app.h"
#include "modbus.h"
#include "main.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// 1. 定义modbus主站句柄，全局
modbusHandler_t hModbusMaster;
extern UART_HandleTypeDef huart3;
#define MODBUS_RS485_UART        &huart3
mb_handle_t *mb_handle = NULL;

/*******************************静态函数********************** */
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

/**
 * @brief IDLE 状态处理
 * @return 1=找到了可执行的请求（写或读轮询），状态已切到 TX
 *         0=当前没有任何可执行任务
 */
static uint8_t mb_poll_idle()
{
    // ========== 1. 优先检查外部写请求（头插，抢占正常读轮询）==========
    mb_write_request_t write_req;
    if (osMessageQueueGet(mb_handle->xWriteReqQueue, &write_req, NULL, 0) == osOK)
    {
        // 写入值拷贝到独立缓冲，pending_write 指向它
        // 这里用 static 局部缓冲——因为 poll 任务是单线程，一次只会有一个 pending
        static mb_write_request_t s_pending_write;
        s_pending_write = write_req;   // 值拷贝

        mb_handle->pending_write = &s_pending_write;
        mb_handle->waiting_is_write = 1;
        mb_handle->waiting_slave = NULL;
        mb_handle->state = MB_STATE_TX;
        return 1;
    }

    // ========== 2. 正常读轮询 ==========
    uint32_t now = osKernelGetTickCount();
    for (uint8_t i = 0; i < mb_handle->slave_max_num; i++)
    {
        mb_slave_runtime_t *slave = &mb_handle->slave_list[i];

        if (slave->valid == 0 || slave->telegram.enable == 0) continue;

        // 阶梯冷却判断
        if (slave->consecutive_fail > 0)
        {
            uint32_t cooling = mb_get_backoff_cooling_ms(slave->consecutive_fail);
            if ((now - slave->last_trigger_tick) < cooling)
            {
                continue;
            }
        }
        // 正常采集周期判断
        if ((now - slave->last_trigger_tick) < slave->telegram.collect_interval_ms) continue;

        // 选中，发起新一轮采集
        mb_handle->waiting_slave = slave;
        mb_handle->waiting_is_write = 0;
        slave->last_trigger_tick = now;
        mb_handle->state = MB_STATE_TX;
        return 1;
    }

    return 0;  // 当前没有任何可执行任务
}

static void mb_poll_tx()
{
    // ========== 写请求路径：构造 telegram 后 ModbusQueryInject 头插 ==========
    if (mb_handle->waiting_is_write)
    {
        mb_write_request_t *wr = mb_handle->pending_write;
        if (wr == NULL)
        {
            mb_handle->state = MB_STATE_IDLE;
            return;
        }

        // 构造写请求 telegram（u16reg 指向 pending_write 内部的 write_values）
        modbus_t tg = {
            .u8id       = wr->slave_id,
            .u8fct      = wr->func_code,
            .u16RegAdd  = wr->reg_addr,
            .u16CoilsNo = wr->reg_count,
            .u16reg     = wr->write_values,
            .collect_interval_ms = 0,
            .enable     = 1,
            .retry_cnt  = 0,
        };

        ModbusQueryInject(&hModbusMaster, tg);   // 头插 + 清队列（队列为空，安全）
        mb_handle->state = MB_STATE_WAIT_ACK;
        return;
    }

    // ========== 正常读轮询路径（原有逻辑）==========
    mb_slave_runtime_t *slave = mb_handle->waiting_slave;
    if (slave == NULL)
    {
        mb_handle->state = MB_STATE_IDLE;
        return;
    }

    // ✅ 队列满时回退到 IDLE，避免静默丢失请求
    if (osMessageQueueGetCount(hModbusMaster.QueueTelegramHandle) >= MAX_TELEGRAMS)
    {
        MB_LOGW("queue full, skip slave %d", slave->telegram.u8id);
        mb_handle->state = MB_STATE_IDLE;
        mb_handle->waiting_slave = NULL;
        return;
    }

    ModbusQuery(&hModbusMaster, slave->telegram);

    mb_handle->state = MB_STATE_WAIT_ACK;
}

static void mb_poll_wait_ack()
{
    // ✅ 改为 portMAX_DELAY，超时完全交给移植库的 xTimerTimeout
    uint32_t notify_val = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    mb_errot_t err = (mb_errot_t)(int32_t)notify_val;

    // ========== 写请求的应答处理（不走读轮询的重试/冷却逻辑）==========
    if (mb_handle->waiting_is_write)
    {
        mb_write_request_t *wr = mb_handle->pending_write;
        if (wr != NULL)
        {
            // 写请求应答不需要解析寄存器数据（移植库 FC6/FC16 不写 u16reg）
            if (wr->result_err_out != NULL)
            {
                *wr->result_err_out = err;   // ✅ 直接写调用者传入的结果指针
            }

            // 打印日志
            if (err == ERR_OK_QUERY)
            {
                MB_LOGI("write OK: slave=%d, FC=%d, addr=%u, count=%u",
                        wr->slave_id, wr->func_code, wr->reg_addr, wr->reg_count);
            }
            else
            {
                MB_LOGE("write FAIL: slave=%d, FC=%d, err=%d",
                        wr->slave_id, wr->func_code, err);
            }

            // 通知调用者（如果传了信号量）
            if (wr->result_sem != NULL)
            {
                osSemaphoreRelease(wr->result_sem);
            }
        }

        // 清理 pending 状态
        mb_handle->pending_write = NULL;
        mb_handle->waiting_is_write = 0;
        mb_handle->state = MB_STATE_IDLE;
        return;
    }

    // ========== 以下是正常读轮询的应答处理（原有逻辑）==========
    mb_slave_runtime_t *slave = mb_handle->waiting_slave;
    if (slave == NULL)
    {
        mb_handle->state = MB_STATE_IDLE;
        return;
    }
    slave->collect_status = err;
    switch(err)
    {
        case ERR_OK_QUERY:
            {
                // ✅ 成功收到应答、CRC校验通过！数据已由移植库写入 telegram.u16reg (= reg_data_buf)
                MB_LOGI("read ok: slave=%d, reg0=%d", slave->telegram.u8id, slave->telegram.u16reg[0]);

                // ✅ 加锁标记数据有效，MQTT 任务读 reg_data_buf 时必须拿同一把锁
                osMutexAcquire(mb_handle->xSlaveDataMutex, osWaitForever);
                slave->valid_point = slave->telegram.u16CoilsNo;  // 有效数据长度 = 请求的寄存器数
                slave->data_valid = 1;
                osMutexRelease(mb_handle->xSlaveDataMutex);

                // 重置连续失败计数, 退出冷却退避, 标记数据更新
                slave->fail_count = 0;
                slave->consecutive_fail = 0;
                slave->is_online = 1;
                slave->collect_status = 1;

                mb_handle->state = MB_STATE_IDLE;
                mb_handle->waiting_slave = NULL;
            }
            break;
        case ERR_TIME_OUT:
            {
                slave->is_online = 0;
                slave->fail_count++;

                if (slave->fail_count <= slave->telegram.retry_cnt)
                {
                    // 本轮还有重试次数，立刻切TX重发
                    mb_handle->state = MB_STATE_TX;
                }
                else
                {
                    // ✅ 本轮所有重试耗尽：连续失败计数 +1
                    if (slave->consecutive_fail < 255)
                    {
                        slave->consecutive_fail++;
                    }
                    slave->fail_count = 0;   // ✅ 本轮采集结束，重试计数清零，不留脏值
                    mb_handle->state = MB_STATE_IDLE;
                    mb_handle->waiting_slave = NULL;
                 }
            }
            break;
        
        default:
            {
                MB_LOGE("read err: %d", err);
            }
            mb_handle->state = MB_STATE_ERR;
            break;
    }
}

static void mb_poll_err()
{
    // 错误状态，等待下一次轮询
    // 这里可以添加错误处理逻辑，比如记录日志、发送错误通知等

    mb_handle->state = MB_STATE_IDLE;
    mb_handle->waiting_slave = NULL;
}

/*******************************静态函数************************/




void Modbus_Init(void)
{
    // 初始化 Modbus 库
    // 填充主站配置
    hModbusMaster.uModbusType = MB_MASTER;
    hModbusMaster.port         = MODBUS_RS485_UART;         // 你的485串口句柄
    hModbusMaster.u8id         = 0;                // modbus主站ID强制为0
    hModbusMaster.EN_Port      = RS485_DIR_GPIO_Port;            // DE/RE控制GPIO端口
    hModbusMaster.EN_Pin       = RS485_DIR_Pin;       // DE/RE控制引脚
    hModbusMaster.xTypeHW      = USART_HW;         // 使用单字节中断模式，先不用DMA
    hModbusMaster.u16timeOut   = TIMEOUT_MODBUS;   // 超时tick，来自ModbusConfig.h

    // 创建库内部FreeRTOS资源(任务、队列、定时器)
    ModbusInit(&hModbusMaster);

    // 初始化从站句柄
    mb_handle_init();

    // 手动添加从站 mb_add_slave(modbus_t *cfg);


    // 开启串口接收，正式开始485硬件通信
    ModbusStart(&hModbusMaster);

}

void vModbusPollTask(void* argument)
{
    // 轮询 Modbus 任务

    for(;;)
    {
        mb_poll_slave();

        // ✅ 仅当 IDLE 且无事可做时才 delay 让出 CPU
        // WAIT_ACK 内部已经 block 在 portMAX_DELAY，不需要额外 delay
        // TX 状态只是立即投递给移植库 master 任务，也不需要
        if (mb_handle->idle_no_work)
        {
            osDelay(1);  // 让出 1 tick（通常 1ms），避免空转占满 CPU
        }
    }

    //ModbusQueryInject(&hModbusMaster, telegram);

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
            // mb_poll_idle 返回 1=找到任务(切到TX)，0=无事可做
            mb_handle->idle_no_work = (mb_poll_idle() == 0) ? 1 : 0;
            break;
        }

        case MB_STATE_TX:
        {
            mb_poll_tx();
            mb_handle->idle_no_work = 0;
            break;
        }

        case MB_STATE_WAIT_ACK:
        {
            mb_poll_wait_ack();   // 内部 block 在 portMAX_DELAY，让出 CPU 不是问题
            mb_handle->idle_no_work = 0;
            break;
        }

        case MB_STATE_ERR:
        {
            mb_poll_err();
            mb_handle->idle_no_work = 0;
            break;
        }
        default:
        {
            mb_handle->state = MB_STATE_IDLE;
            mb_handle->waiting_slave = NULL;
            mb_handle->idle_no_work = 0;
            break;
        }
    }
    return MB_STATUS_OK;
}

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
    // 4. dma_rx_buf 已经 memset 清零

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

    // ✅ 外部写请求队列：MQTT下发配置/运行写操作走这个通道
    mb_handle->xWriteReqQueue = osMessageQueueNew(MB_WRITE_QUEUE_DEPTH,
                                                  sizeof(mb_write_request_t), NULL);
    if (mb_handle->xWriteReqQueue == NULL)
    {
        free(mb_handle->slave_list);
        free(mb_handle);
        return MB_INIT_STATUS_ERR;
    }

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
        if (mb_handle->slave_list[i].telegram.u8id == slave_id  && mb_handle->slave_list[i].valid == 1)
        {
            return &mb_handle->slave_list[i];
        }
    }
    return NULL;
}

mb_handle_t* mb_handle_get(void)
{
    return mb_handle;
}

void mb_set_config_change_cb(mb_cfg_change_cb_t cb, void *ctx)
{
    if (mb_handle == NULL) return;
    mb_handle->cfg_change_cb  = cb;
    mb_handle->cfg_change_ctx = ctx;
}

/**
 * @brief 添加从站
 * 
 * @param cfg 从站配置指针
 * @return mb_status_e 添加状态
 */
mb_status_e mb_add_slave(modbus_t *cfg)
{
    if (mb_handle == NULL)
    {
        return MB_HANDLE_STATUS_ERR;
    }

    // ✅ 边界保护：寄存器数不能超过 reg_data_buf 容量
    if (cfg->u16CoilsNo > MB_REG_MAX_PER_SLAVE)
    {
        MB_LOGE("reject slave %d: u16CoilsNo=%u > %u",
                cfg->u8id, cfg->u16CoilsNo, MB_REG_MAX_PER_SLAVE);
        return MB_BUILD_REQUEST_ERR;  // 借用这个错误码表示配置非法
    }

    // 检查从站ID是否已存在
    mb_slave_runtime_t *slave = mb_get_slave_runtime(cfg->u8id);
    if (slave != NULL)
    {
        // 更新配置
        memcpy(&slave->telegram, cfg, sizeof(modbus_t));

        //将所有缓存指向reg_data_buf
        slave->telegram.u16reg = slave->reg_data_buf;

        slave->telegram.u32CurrentTask = NULL;
        slave->valid = 1;

        // ✅ 触发配置变更回调（UPDATE）
        if (mb_handle->cfg_change_cb)
        {
            mb_handle->cfg_change_cb(MB_CFG_CHANGE_UPDATE, slave->telegram.u8id,
                                      mb_handle->cfg_change_ctx);
        }
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
    memcpy(&slave->telegram, cfg, sizeof(modbus_t));
    //将所有缓存指向reg_data_buf
    slave->telegram.u16reg = slave->reg_data_buf;
    slave->telegram.u32CurrentTask = NULL;
    slave->valid = 1;
    mb_handle->slave_actual_num++;

    // ✅ 触发配置变更回调（ADD）
    MB_LOGI("slave added: id=%d, FC=%d, addr=%u, count=%u",
            slave->telegram.u8id, slave->telegram.u8fct,
            slave->telegram.u16RegAdd, slave->telegram.u16CoilsNo);
    if (mb_handle->cfg_change_cb)
    {
        mb_handle->cfg_change_cb(MB_CFG_CHANGE_ADD, slave->telegram.u8id,
                                  mb_handle->cfg_change_ctx);
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

    MB_LOGI("slave deleted: id=%d", slave_id);

    // ✅ 触发配置变更回调（DELETE）
    if (mb_handle->cfg_change_cb)
    {
        mb_handle->cfg_change_cb(MB_CFG_CHANGE_DELETE, slave_id, mb_handle->cfg_change_ctx);
    }

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

    // ✅ 用临时变量接住，防止 realloc 失败时旧指针丢失导致内存泄漏
    uint8_t new_cap = mb_handle->slave_max_num + MB_SLAVE_INIT_CAP;
    mb_slave_runtime_t *new_list = realloc(mb_handle->slave_list, sizeof(mb_slave_runtime_t) * new_cap);
    if(new_list == NULL)
    {
        return MB_REALLOC_STATUS_ERR;  // 旧的 slave_list 还在，不丢失
    }
    mb_handle->slave_list = new_list;
    mb_handle->slave_max_num = new_cap;

    return MB_STATUS_OK;
}

/**
 * @brief 外部注入写请求（FC6 / FC16）
 *
 * 由 MQTT / 配置管理任务调用，请求投入队列后 poll 任务在 IDLE 时优先取出，
 * 用 ModbusQueryInject 头插（会清空移植库队列，但 poll 单槽模式下队列为空，安全）。
 */
mb_status_e mb_inject_write(uint8_t slave_id,
                            uint8_t func_code,
                            uint16_t reg_addr,
                            uint16_t reg_count,
                            const uint16_t *write_values,
                            osSemaphoreId_t result_sem,
                            mb_errot_t *result_err_out)
{
    if (mb_handle == NULL)          return MB_HANDLE_STATUS_ERR;
    if (func_code != MB_FC_WRITE_REGISTER && func_code != MB_FC_WRITE_MULTIPLE_REGISTERS)
                                    return MB_NOT_SUPPORT_FC_ERR;
    if (write_values == NULL)       return MB_BUILD_REQUEST_ERR;
    if (reg_count > MB_REG_MAX_PER_SLAVE)
                                    return MB_BUILD_REQUEST_ERR;

    // 构造请求（写入值直接拷贝，无指针生命周期风险）
    mb_write_request_t wr;
    memset(&wr, 0, sizeof(wr));
    wr.slave_id       = slave_id;
    wr.func_code      = func_code;
    wr.reg_addr       = reg_addr;
    wr.reg_count      = reg_count;
    wr.result_sem     = result_sem;
    wr.result_err_out = result_err_out;

    for (uint16_t i = 0; i < reg_count; i++)
    {
        wr.write_values[i] = write_values[i];
    }

    // 投入队列（prio=0，不等待，满了立即返回）
    if (osMessageQueuePut(mb_handle->xWriteReqQueue, &wr, 0, 0) != osOK)
    {
        MB_LOGW("write req queue full, slave=%d", slave_id);
        return MB_QUEUE_FULL_ERR;
    }

    // 同步等待结果：poll 任务处理完后 release 信号量，同时填 *result_err_out
    if (result_sem != NULL)
    {
        osSemaphoreAcquire(result_sem, osWaitForever);
        // 信号量返回后，*result_err_out 已经由 poll 任务填好
    }

    return MB_STATUS_OK;
}

/* ============= MQTT 方 / 存储方 统一接口 实现 ============= */

uint8_t mb_for_each_valid_slave(mb_slave_iter_cb_t cb, void *ctx)
{
    if (mb_handle == NULL || cb == NULL) return 0;

    uint8_t count = 0;

    // 配置锁保护从站列表结构（valid/enable 这些字段）
    osMutexAcquire(mb_handle->xSlaveCfgMutex, osWaitForever);

    for (uint8_t i = 0; i < mb_handle->slave_max_num; i++)
    {
        mb_slave_runtime_t *s = &mb_handle->slave_list[i];
        if (!s->valid || !s->telegram.enable) continue;

        count++;

        // 回调：回调里只做只读操作（如遍历、快照拷贝），不要再调 mb_add_slave/mb_delete_slave
        if (cb(s, ctx) != 0)
        {
            break;  // 回调返回非 0，立即停止
        }
    }

    osMutexRelease(mb_handle->xSlaveCfgMutex);

    return count;
}

mb_status_e mb_take_slave_snapshot(uint8_t slave_id, mb_slave_snapshot_t *snap_out)
{
    if (mb_handle == NULL || snap_out == NULL) return MB_HANDLE_STATUS_ERR;

    // 先用配置锁定位从站（避免遍历时刚好被 mb_delete_slave 标记无效）
    osMutexAcquire(mb_handle->xSlaveCfgMutex, osWaitForever);
    mb_slave_runtime_t *s = mb_get_slave_runtime(slave_id);
    if (s == NULL || !s->telegram.enable)
    {
        osMutexRelease(mb_handle->xSlaveCfgMutex);
        return MB_SLAVE_NOT_FOUND_ERR;
    }
    osMutexRelease(mb_handle->xSlaveCfgMutex);

    // 数据锁保护 reg_data_buf（poll 任务写，这里拷贝）
    osMutexAcquire(mb_handle->xSlaveDataMutex, osWaitForever);

    snap_out->slave_id   = s->telegram.u8id;
    snap_out->is_online  = s->is_online;
    snap_out->reg_addr   = s->telegram.u16RegAdd;
    snap_out->valid_point = s->valid_point;  // 有效数据长度

    uint16_t copy_len = (s->valid_point > MB_REG_MAX_PER_SLAVE)
                        ? MB_REG_MAX_PER_SLAVE : s->valid_point;
    memcpy(snap_out->data, s->reg_data_buf, sizeof(uint16_t) * copy_len);

    osMutexRelease(mb_handle->xSlaveDataMutex);

    return MB_STATUS_OK;
}

void mb_notify_reload_complete(void)
{
    if (mb_handle == NULL) return;

    MB_LOGI("slave reload complete: %u slaves", mb_handle->slave_actual_num);

    if (mb_handle->cfg_change_cb)
    {
        mb_handle->cfg_change_cb(MB_CFG_CHANGE_RELOAD, 0, mb_handle->cfg_change_ctx);
    }
}