/**
 * @file mb_poll.c
 * @brief Modbus Poll 状态机 + 硬件初始化 + 任务入口
 *
 *  硬实时路径，单槽一问一答状态机：
 *    IDLE → TX → WAIT_ACK → IDLE（或重试 → TX）
 *
 *  不做 malloc/free，不做从站 CRUD，这些在 mb_slave.c 里。
 */

#include "mb_internal.h"
#include "main.h"
#include "mb_pointmap.h"
#include <stdio.h>

/* ========= 全局变量定义（mb_internal.h 里 extern 给其他文件）========= */
modbusHandler_t hModbusMaster;
mb_handle_t *mb_handle = NULL;

extern UART_HandleTypeDef huart3;
#define MODBUS_RS485_UART  &huart3

/* ========= 内部静态函数 ========= */

/**
 * @brief 根据连续失败次数得到本次冷却时长，带封顶
 */
static uint32_t mb_get_backoff_cooling_ms(uint8_t consecutive_fail)
{
    if (consecutive_fail == 1)      return 5000;
    else if (consecutive_fail == 2) return 10000;
    else                            return 20000;  // >=3 封顶
}

/**
 * @brief IDLE 状态处理
 * @return 1=找到了可执行的请求，状态已切到 TX
 *         0=当前没有任何可执行任务
 */
static uint8_t mb_poll_idle(void)
{
    // ========== 1. 优先检查外部写请求（头插，抢占正常读轮询）==========
    mb_write_request_t write_req;
    if (osMessageQueueGet(mb_handle->xWriteReqQueue, &write_req, NULL, 0) == osOK)
    {
        static mb_write_request_t s_pending_write;   // poll 单线程，static 足够
        s_pending_write = write_req;

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
        if (!slave->valid || !slave->telegram.enable) continue;

        // 阶梯冷却
        if (slave->consecutive_fail > 0)
        {
            uint32_t cooling = mb_get_backoff_cooling_ms(slave->consecutive_fail);
            if ((now - slave->last_trigger_tick) < cooling) continue;
        }
        // 采集周期
        if ((now - slave->last_trigger_tick) < slave->telegram.collect_interval_ms) continue;

        mb_handle->waiting_slave = slave;
        mb_handle->waiting_is_write = 0;
        slave->last_trigger_tick = now;
        mb_handle->state = MB_STATE_TX;
        return 1;
    }

    return 0;
}

static void mb_poll_tx(void)
{
    if (mb_handle->waiting_is_write)
    {
        // 写请求路径：构造 telegram 后头插
        mb_write_request_t *wr = mb_handle->pending_write;
        if (wr == NULL) { mb_handle->state = MB_STATE_IDLE; return; }

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
        ModbusQueryInject(&hModbusMaster, tg);
        mb_handle->state = MB_STATE_WAIT_ACK;
        return;
    }

    // 正常读轮询路径
    mb_slave_runtime_t *slave = mb_handle->waiting_slave;
    if (slave == NULL) { mb_handle->state = MB_STATE_IDLE; return; }

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

static void mb_poll_wait_ack(void)
{
    uint32_t notify_val = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    mb_errot_t err = (mb_errot_t)(int32_t)notify_val;

    // ========== 写请求应答 ==========
    if (mb_handle->waiting_is_write)
    {
        mb_write_request_t *wr = mb_handle->pending_write;
        if (wr != NULL)
        {
            if (wr->result_err_out != NULL) *wr->result_err_out = err;

            if (err == ERR_OK_QUERY)
                MB_LOGI("write OK: slave=%d, FC=%d, addr=%u, count=%u",
                        wr->slave_id, wr->func_code, wr->reg_addr, wr->reg_count);
            else
                MB_LOGE("write FAIL: slave=%d, FC=%d, err=%d",
                        wr->slave_id, wr->func_code, err);

            if (wr->result_sem != NULL) osSemaphoreRelease(wr->result_sem);
        }
        mb_handle->pending_write = NULL;
        mb_handle->waiting_is_write = 0;
        mb_handle->state = MB_STATE_IDLE;
        return;
    }

    // ========== 正常读轮询应答 ==========
    mb_slave_runtime_t *slave = mb_handle->waiting_slave;
    if (slave == NULL) { mb_handle->state = MB_STATE_IDLE; return; }
    slave->collect_status = err;

    switch (err)
    {
        case ERR_OK_QUERY:
        {
            MB_LOGI("read ok: slave=%d, reg0=%d", slave->telegram.u8id, slave->telegram.u16reg[0]);

            osMutexAcquire(mb_handle->xSlaveDataMutex, osWaitForever);
            slave->valid_point = slave->telegram.u16CoilsNo;
            slave->data_valid = 1;
            osMutexRelease(mb_handle->xSlaveDataMutex);

            slave->fail_count = 0;
            slave->consecutive_fail = 0;
            slave->is_online = 1;
            slave->collect_status = 1;

            mb_handle->state = MB_STATE_IDLE;
            mb_handle->waiting_slave = NULL;
            break;
        }
        case ERR_TIME_OUT:
        {
            slave->is_online = 0;
            slave->fail_count++;

            if (slave->fail_count <= slave->telegram.retry_cnt)
            {
                mb_handle->state = MB_STATE_TX;
            }
            else
            {
                if (slave->consecutive_fail < 255) slave->consecutive_fail++;
                slave->fail_count = 0;
                mb_handle->state = MB_STATE_IDLE;
                mb_handle->waiting_slave = NULL;
            }
            break;
        }
        default:
        {
            MB_LOGE("read err: %d", err);
            mb_handle->state = MB_STATE_ERR;
            break;
        }
    }
}

static void mb_poll_err(void)
{
    mb_handle->state = MB_STATE_IDLE;
    mb_handle->waiting_slave = NULL;
}

/* ========= 对外 API ========= */

/**
 * @brief Modbus 整体初始化（硬件 + 移植库 + 句柄）
 */
void Modbus_Init(void)
{
    MB_LOGE("Modbus初始化");

    // 填充主站配置
    hModbusMaster.uModbusType = MB_MASTER;
    hModbusMaster.port         = MODBUS_RS485_UART;
    hModbusMaster.u8id         = 0;   // 主站 ID 强制为 0
    hModbusMaster.EN_Port      = RS485_DIR_GPIO_Port;
    hModbusMaster.EN_Pin       = RS485_DIR_Pin;
    hModbusMaster.xTypeHW      = USART_HW;
    hModbusMaster.u16timeOut   = TIMEOUT_MODBUS;

    ModbusInit(&hModbusMaster);   // 创建移植库内部 FreeRTOS 资源

    mb_handle_init();   // 初始化从站句柄（在 mb_slave.c）

    ModbusStart(&hModbusMaster);  // 开启串口接收
}

/**
 * @brief Modbus Poll 任务入口（在 app_modules.c 里注册）
 */
void vModbusPollTask(void *argument)
{
    for (;;)
    {
        mb_poll_slave();

        // 只有 IDLE 且无事可做时才 delay；WAIT_ACK 内部已 block
        if (mb_handle->idle_no_work)
        {
            osDelay(1);
        }
    }
}

/**
 * @brief 状态机推进（由 poll 任务循环持续调用）
 */
mb_status_e mb_poll_slave(void)
{
    switch (mb_handle->state)
    {
        case MB_STATE_IDLE:
            mb_handle->idle_no_work = (mb_poll_idle() == 0) ? 1 : 0;
            break;

        case MB_STATE_TX:
            mb_poll_tx();
            mb_handle->idle_no_work = 0;
            break;

        case MB_STATE_WAIT_ACK:
            mb_poll_wait_ack();
            mb_handle->idle_no_work = 0;
            break;

        case MB_STATE_ERR:
            mb_poll_err();
            mb_handle->idle_no_work = 0;
            break;

        default:
            mb_handle->state = MB_STATE_IDLE;
            mb_handle->waiting_slave = NULL;
            mb_handle->idle_no_work = 0;
            break;
    }
    return MB_STATUS_OK;
}
