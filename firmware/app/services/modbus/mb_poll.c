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
#include "app_monitor.h"
#include "upgrade_quiesce.h"
#include <stdio.h>


/* ========= 全局变量定义（mb_internal.h 里 extern 给其他文件）========= */
modbusHandler_t hModbusMaster;
mb_handle_t *mb_handle = NULL;

extern UART_HandleTypeDef huart3;
#define MODBUS_RS485_UART  &huart3

/* 接收链路诊断计数（定义在 UARTCallback.c） */
// extern volatile uint32_t mb_dbg_rx_bytes;
// extern volatile uint32_t mb_dbg_uart_err;
// extern volatile uint32_t mb_dbg_uart_err_cnt;

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
        if (mb_runtime_paused() || !UpgradeQuiesce_AllowsMutation() ||
            !mb_write_pending(&write_req))
        {
            mb_finish_write(&write_req, ERR_UPGRADE_BUSY);
            return 0;
        }
        if (!mb_runtime_poll_begin())
        {
            mb_finish_write(&write_req, ERR_UPGRADE_BUSY);
            return 0;
        }
        static mb_write_request_t s_pending_write;   // poll 单线程，static 足够
        s_pending_write = write_req;

        mb_handle->pending_write = &s_pending_write;
        mb_handle->waiting_is_write = 1;
        mb_handle->waiting_slave = NULL;
        mb_handle->state = MB_STATE_TX;
        return 1;
    }

    if (!mb_runtime_poll_begin()) return 0;

    // ========== 2. 正常读轮询 ==========
    uint32_t now = osKernelGetTickCount();
    mb_slave_runtime_t *best_slave = NULL;
    uint32_t best_age = 0;

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

        // 在"已到期"的从站里选最久没采集的那个：
        // 若按数组顺序遇到第一个就返回，排在前面的快周期从站会把后面的从站饿死
        uint32_t age = now - slave->last_trigger_tick;
        if (best_slave == NULL || age > best_age)
        {
            best_slave = slave;
            best_age = age;
        }
    }

    if (best_slave == NULL) return 0;

    mb_handle->waiting_slave = best_slave;
    mb_handle->waiting_is_write = 0;
    best_slave->last_trigger_tick = now;
    mb_handle->state = MB_STATE_TX;
    return 1;
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
        /* 队列满 = master 任务没在取帧，打印现场便于定位（只在此异常时输出） */
        MB_LOGW("queue full, skip slave %d | q=%u master=%d libstate=%d in=%u err=%u rx=%u uart=%d",
                slave->telegram.u8id,
                (unsigned int)osMessageQueueGetCount(hModbusMaster.QueueTelegramHandle),
                (int)eTaskGetState((TaskHandle_t)hModbusMaster.myTaskModbusAHandle),
                (int)hModbusMaster.i8state,
                (unsigned int)hModbusMaster.u16InCnt,
                (unsigned int)hModbusMaster.u16errCnt,
                (unsigned int)hModbusMaster.xBufferRX.u8available,
                (int)HAL_UART_GetState(hModbusMaster.port));
        mb_handle->state = MB_STATE_IDLE;
        mb_handle->waiting_slave = NULL;
        return;
    }

    ModbusQuery(&hModbusMaster, slave->telegram);
    mb_handle->state = MB_STATE_WAIT_ACK;
}

static void mb_poll_wait_ack(void)
{
    /* 等应答前必须保证 RX 中断是开的：HAL 一旦遇到 ORE/FE/NE 会关掉 RXNEIE，
     * 如果没人重启，本轮和之后所有应答都会被丢弃（现场表现就是
     * rxneie=0 + 每帧都 1200ms 超时）。这里做一次兜底重启。 */
    if (hModbusMaster.xTypeHW == USART_HW &&
        hModbusMaster.port->RxState == HAL_UART_STATE_READY &&
        (hModbusMaster.port->Instance->CR1 & USART_CR1_RXNEIE) == 0U)
    {
        HAL_UART_Receive_IT(hModbusMaster.port, &hModbusMaster.dataRX, 1);
    }

    // 限时等待：正常完成通知只可能是错误码（-8..-11），0 表示"没收到任何通知"
    uint32_t notify_val = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TIMEOUT_MODBUS + 200U));
    if (notify_val == 0U)
    {
        // master 侧卡住（发送/中断异常）时，这里绝不能无限阻塞，按超时处理并复位状态机
        MB_LOGE("wait_ack: no notify in %u ms, result UNKNOWN",
                (unsigned int)(TIMEOUT_MODBUS + 200U));
        notify_val = (uint32_t)(int32_t)ERR_UNKNOWN;
        mb_runtime_fail();
    }
    mb_errot_t err = (mb_errot_t)(int32_t)notify_val;

    // ========== 写请求应答 ==========
    if (mb_handle->waiting_is_write)
    {
        mb_write_request_t *wr = mb_handle->pending_write;
        if (wr != NULL)
        {
            mb_errot_t result = err;
            if (err != ERR_OK_QUERY && err != ERR_UPGRADE_BUSY &&
                err != ERR_BAD_SLAVE_ID && err != ERR_NOT_MASTER)
                result = ERR_UNKNOWN;
            mb_finish_write(wr, result);

            if (err == ERR_OK_QUERY)
                MB_LOGI("write OK: slave=%d, FC=%d, addr=%u, count=%u",
                        wr->slave_id, wr->func_code, wr->reg_addr, wr->reg_count);
            else
                MB_LOGE("write result: slave=%d, FC=%d, err=%d",
                        wr->slave_id, wr->func_code, result);

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
            // FC01/02 读线圈/离散输入：N 个 bit 打包成 ceil(N/16) 个 uint16
            // FC03/04 读寄存器：N 个寄存器 = N 个 uint16
            if (slave->telegram.u8fct == MB_FC_READ_COILS ||
                slave->telegram.u8fct == MB_FC_READ_DISCRETE_INPUT)
            {
                slave->valid_point = (slave->telegram.u16CoilsNo + 15U) / 16U;
                // get_FC1 按字节填充，N%16!=0 时最后一个打包字的高位是上轮残留，掩码清掉
                uint8_t rem = (uint8_t)(slave->telegram.u16CoilsNo % 16U);
                if (rem != 0U)
                {
                    slave->reg_data_buf[slave->valid_point - 1U] &=
                        (uint16_t)((1U << rem) - 1U);
                }
            }
            else
            {
                slave->valid_point = slave->telegram.u16CoilsNo;
            }
            slave->data_valid = 1;
            osMutexRelease(mb_handle->xSlaveDataMutex);

            slave->fail_count = 0;
            slave->consecutive_fail = 0;
            slave->is_online = 1;

            mb_handle->state = MB_STATE_IDLE;
            mb_handle->waiting_slave = NULL;
            break;
        }
        case ERR_TIME_OUT:
        {
            slave->is_online = 0;
            slave->fail_count++;

            MB_LOGW("TIMEOUT slave=%d fc=%d addr=%u qty=%u, fail_count=%d | rxneie=%d rxstate=%d",
                    slave->telegram.u8id, slave->telegram.u8fct,
                    slave->telegram.u16RegAdd, slave->telegram.u16CoilsNo, slave->fail_count,
                    (int)((huart3.Instance->CR1 & USART_CR1_RXNEIE) != 0),
                    (int)huart3.RxState);

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
    MB_LOGI("Modbus初始化");

    // 填充主站配置
    hModbusMaster.uModbusType = MB_MASTER;
    hModbusMaster.port         = MODBUS_RS485_UART;
    hModbusMaster.u8id         = 0;   // 主站 ID 强制为 0
    hModbusMaster.EN_Port      = RS485_DIR_GPIO_Port;  // 485模块自动切换，无需 DE/RE 控制
    hModbusMaster.EN_Pin       = RS485_DIR_Pin;
    hModbusMaster.xTypeHW      = USART_HW;
    hModbusMaster.u16timeOut   = TIMEOUT_MODBUS;

    ModbusInit(&hModbusMaster);   // 创建移植库内部 FreeRTOS 资源

    mb_handle_init();   // 初始化从站句柄（在 mb_slave.c）

    if (mb_handle == NULL) return;
    ModbusStart(&hModbusMaster);  // 开启串口接收
    if (!mb_runtime_init()) MB_LOGE("upgrade runtime init failed");
}

/**
 * @brief Modbus Poll 任务入口（在 app_modules.c 里注册）
 */
void vModbusPollTask(void *argument)
{
    (void)argument;
    if (!mb_runtime_start_health())
    {
        Monitor_ReportFatal(APP_MODULE_MODBUS);
        return;
    }
    //test_add_three_slaves();
    for (;;)
    {
        mb_poll_slave();
        if (mb_handle->state == MB_STATE_IDLE) mb_runtime_poll_end();
        mb_runtime_report();
        if (mb_handle->idle_no_work)
        {
            osDelay(1);
        }

        //test_read_snapshot();
        //test_parse_points();
        //test_write_operations();
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
