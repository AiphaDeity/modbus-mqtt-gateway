/**
 * @file mb_runtime.c
 * @brief Modbus升级暂停、发送准入和任务进展。
 */
#include "mb_internal.h"
#include "app_monitor.h"
#include "upgrade_quiesce.h"

#define MB_PROGRESS_MS 3000U

static bool s_ready;
static bool s_paused;
static bool s_faulted;
static bool s_poll_active;
static bool s_master_active;
static bool s_master_seen;
static uint32_t s_cfg_active;
static osThreadId_t s_cfg_owner;
static uint32_t s_cfg_started;
static uint32_t s_master_tick;
static uint32_t s_started_tick;
static osThreadId_t s_poll_owner;

/** @brief 按本次启动固定配置检查从站地址；未配置不允许总线作业。 */
bool mb_runtime_configured_slave(uint8_t slave_id)
{
    const app_config_t *cfg = AppConfig_Get();
    if (cfg == NULL) return false;
    for (uint32_t i = 0; i < cfg->mb_slave_count; i++)
        if (cfg->mb_slave_addr[i] == slave_id) return true;
    return false;
}

/** @brief 判断当前操作是否为会改变从站状态的写功能码。 */
static bool mb_is_write(uint8_t func)
{
    return func == MB_FC_WRITE_COIL || func == MB_FC_WRITE_REGISTER ||
           func == MB_FC_WRITE_MULTIPLE_COILS || func == MB_FC_WRITE_MULTIPLE_REGISTERS;
}

/** @brief 检查队列、在途处理及UART发送均已结束；调用方持短临界区。 */
static bool mb_runtime_settled(void)
{
    return s_ready && !s_faulted && !s_poll_active && !s_master_active &&
           s_cfg_active == 0U && mb_handle->pending_write == NULL &&
           mb_handle->state == MB_STATE_IDLE && hModbusMaster.i8state == COM_IDLE &&
           osMessageQueueGetCount(mb_handle->xWriteReqQueue) == 0U &&
           osMessageQueueGetCount(hModbusMaster.QueueTelegramHandle) == 0U &&
           mb_write_results_settled() &&
           hModbusMaster.port->gState == HAL_UART_STATE_READY;
}

/** @brief 先关新请求入口；结算阶段由原任务完成在途处理。 */
static UpgradeStageResult mb_runtime_pause(void *context, uint32_t timeout_ms)
{
    (void)context;
    if (timeout_ms == 0U || osThreadGetId() == hModbusMaster.myTaskModbusAHandle ||
        osThreadGetId() == s_poll_owner)
        return UPGRADE_STAGE_MODE;
    taskENTER_CRITICAL();
    s_paused = true;
    bool ready = s_ready && !s_faulted;
    taskEXIT_CRITICAL();
    return ready ? UPGRADE_STAGE_OK : UPGRADE_STAGE_NOT_READY;
}

/** @brief 等待队列明确结算，超时保持暂停；不复位队列或中断已发送的写。 */
static UpgradeStageResult mb_runtime_settle(void *context, uint32_t timeout_ms)
{
    (void)context;
    uint32_t started = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - started) < timeout_ms)
    {
        taskENTER_CRITICAL();
        bool settled = s_paused && mb_runtime_settled();
        bool failed = s_faulted;
        taskEXIT_CRITICAL();
        if (failed) return UPGRADE_STAGE_INTERNAL;
        if (settled) return UPGRADE_STAGE_OK;
        osDelay(1U);
    }
    return UPGRADE_STAGE_TIMEOUT;
}

/** @brief Modbus不持有介质；确认结果已交还且总线空闲后才允许同步完成。 */
static UpgradeStageResult mb_runtime_sync(void *context, uint32_t timeout_ms)
{
    return mb_runtime_settle(context, timeout_ms);
}

/** @brief 在途处理结束后恢复轮询；全局写门控由升级协调器最后开放。 */
static UpgradeStageResult mb_runtime_resume(void *context, uint32_t timeout_ms)
{
    UpgradeStageResult result = mb_runtime_settle(context, timeout_ms);
    if (result == UPGRADE_STAGE_OK)
    {
        taskENTER_CRITICAL();
        s_paused = false;
        taskEXIT_CRITICAL();
    }
    return result;
}

/** @brief 资源就绪后绑定一次真实Modbus暂停角色，不创建任务。 */
bool mb_runtime_init(void)
{
    if (mb_handle == NULL || mb_handle->xWriteReqQueue == NULL ||
        mb_handle->xSlaveCfgMutex == NULL || mb_handle->xSlaveDataMutex == NULL ||
        hModbusMaster.QueueTelegramHandle == NULL || hModbusMaster.ModBusSphrHandle == NULL ||
        hModbusMaster.myTaskModbusAHandle == NULL || hModbusMaster.xTimerTimeout == NULL ||
        hModbusMaster.xTimerT35 == NULL || hModbusMaster.port == NULL ||
        hModbusMaster.port->RxState != HAL_UART_STATE_BUSY_RX)
        return false;
    const UpgradeQuiesceParticipant participant = {
        mb_runtime_pause, mb_runtime_settle, mb_runtime_sync, mb_runtime_resume, NULL
    };
    s_started_tick = HAL_GetTick();
    s_ready = UpgradeQuiesce_Bind(UPGRADE_QUIESCE_MODBUS, &participant) == UPGRADE_STAGE_OK;
    return s_ready;
}

/** @brief 向装配层报告真实初始化结果。 */
bool mb_runtime_ready(void)
{
    return s_ready && !s_faulted;
}

/** @brief 轮询选择请求前占住运行表，防止配置扩容使在途指针失效。 */
bool mb_runtime_poll_begin(void)
{
    taskENTER_CRITICAL();
    bool allowed = s_ready && AppConfig_Get() != NULL &&
                   !s_faulted && !s_paused && s_cfg_active == 0U &&
                   !s_master_active &&
                   osMessageQueueGetCount(hModbusMaster.QueueTelegramHandle) == 0U;
    if (allowed) s_poll_active = true;
    taskEXIT_CRITICAL();
    return allowed;
}

/** @brief 原轮询事务回到IDLE后释放运行表。 */
void mb_runtime_poll_end(void)
{
    taskENTER_CRITICAL();
    s_poll_active = false;
    taskEXIT_CRITICAL();
}

/** @brief 检查本地暂停；试运行仍可执行只读采样。 */
bool mb_runtime_paused(void)
{
    taskENTER_CRITICAL();
    bool paused = s_paused || s_faulted;
    taskEXIT_CRITICAL();
    return paused;
}

/** @brief 配置变更只在总线和轮询空闲时准入，计数覆盖完整调用及回调。 */
bool mb_runtime_config_begin(void)
{
    if (__get_IPSR() != 0U || osKernelGetState() != osKernelRunning) return false;
    if (AppConfig_Get() != NULL) return false; // 已发布的配置仅重启切换，不能旁路热改点表。
    taskENTER_CRITICAL();
    bool continuing = s_cfg_active != 0U && s_cfg_owner == osThreadGetId();
    bool allowed = s_ready && !s_faulted && !s_poll_active && !s_master_active &&
                   (continuing || (!s_paused && s_cfg_active == 0U &&
                                    UpgradeQuiesce_AllowsMutation())) &&
                   osMessageQueueGetCount(hModbusMaster.QueueTelegramHandle) == 0U;
    if (allowed)
    {
        if (s_cfg_active == 0U)
        {
            s_cfg_owner = osThreadGetId();
            s_cfg_started = HAL_GetTick();
        }
        s_cfg_active++;
    }
    taskEXIT_CRITICAL();
    return allowed;
}

/** @brief 配置调用及其通知结束后归还准入计数。 */
void mb_runtime_config_end(void)
{
    taskENTER_CRITICAL();
    if (s_cfg_active != 0U && s_cfg_owner == osThreadGetId())
    {
        s_cfg_active--;
        if (s_cfg_active == 0U) s_cfg_owner = NULL;
    }
    taskEXIT_CRITICAL();
}

/** @brief 统一入库帧队列；检查与入队同处临界区，禁止暂停后迟到入队。 */
bool mb_runtime_enqueue(modbusHandler_t *handler, const modbus_t *telegram, bool front)
{
    taskENTER_CRITICAL();
    bool allowed = handler == &hModbusMaster && osThreadGetId() == s_poll_owner &&
                   s_ready && !s_faulted && !s_paused &&
                   s_cfg_active == 0U && mb_runtime_configured_slave(telegram->u8id) &&
                   (!mb_is_write(telegram->u8fct) || UpgradeQuiesce_AllowsMutation());
    BaseType_t result = pdFAIL;
    if (allowed)
        result = front ? xQueueSendToFront(handler->QueueTelegramHandle, telegram, 0U) :
                         xQueueSendToBack(handler->QueueTelegramHandle, telegram, 0U);
    taskEXIT_CRITICAL();
    return result == pdPASS;
}

/** @brief Master取帧与登记在途原子衔接，不留下队列空但仍将发送的窗口。 */
bool mb_runtime_take_query(modbusHandler_t *handler, modbus_t *telegram)
{
    taskENTER_CRITICAL();
    bool taken = s_ready && handler == &hModbusMaster &&
                 xQueueReceive(handler->QueueTelegramHandle, telegram, 0U) == pdPASS;
    if (taken) s_master_active = true;
    taskEXIT_CRITICAL();
    return taken;
}

/** @brief Master完成上次结果通知或检查了空队列后报告自身进展。 */
void mb_runtime_query_complete(void)
{
    taskENTER_CRITICAL();
    s_master_active = false;
    s_master_seen = true;
    s_master_tick = HAL_GetTick();
    taskEXIT_CRITICAL();
}

/** @brief 最终发送边界重查准入，与拉高方向脚及启动UART不可抢占地衔接。 */
HAL_StatusTypeDef mb_runtime_transmit(modbusHandler_t *handler)
{
    taskENTER_CRITICAL();
    bool write = mb_is_write(handler->u8Buffer[FUNC]);
    bool write_pending = !write || (mb_handle != NULL && mb_handle->waiting_is_write &&
                         mb_handle->pending_write != NULL && mb_write_pending(mb_handle->pending_write));
    bool allowed = handler == &hModbusMaster && s_ready && !s_faulted && !s_paused &&
                   mb_runtime_configured_slave(handler->u8Buffer[0]) &&
                   s_cfg_active == 0U &&
                   write_pending && (!write || UpgradeQuiesce_AllowsMutation());
    HAL_StatusTypeDef result = HAL_BUSY;
    handler->i8lastError = ERR_UPGRADE_BUSY;
    if (allowed)
    {
        if (handler->EN_Port != NULL)
            HAL_GPIO_WritePin(handler->EN_Port, handler->EN_Pin, GPIO_PIN_SET);
#if ENABLE_USART_DMA == 1
        if (handler->xTypeHW == USART_HW_DMA)
            result = HAL_UART_Transmit_DMA(handler->port, handler->u8Buffer, handler->u8BufferSize);
        else
#endif
            result = HAL_UART_Transmit_IT(handler->port, handler->u8Buffer, handler->u8BufferSize);
        if (result != HAL_OK)
        {
            handler->i8lastError = ERR_UNKNOWN;
            s_faulted = true;
        }
    }
    if (result != HAL_OK && handler->EN_Port != NULL)
        HAL_GPIO_WritePin(handler->EN_Port, handler->EN_Pin, GPIO_PIN_RESET);
    taskEXIT_CRITICAL();
    return result;
}

/** @brief 汇总Poll与Master实际进展；从站超时可降级，工作任务停摆则锁存故障。 */
void mb_runtime_report(void)
{
    uint32_t now = HAL_GetTick();
    taskENTER_CRITICAL();
    bool expired = (uint32_t)(now - (s_master_seen ? s_master_tick : s_started_tick)) >= MB_PROGRESS_MS;
    expired = expired || (s_cfg_active != 0U &&
                           (uint32_t)(now - s_cfg_started) >= MB_PROGRESS_MS);
    bool state_valid = mb_handle != NULL && (uint32_t)mb_handle->state <= (uint32_t)MB_STATE_ERR;
    bool uart_ready = s_ready && (s_master_active ||
                       hModbusMaster.port->RxState == HAL_UART_STATE_BUSY_RX);
    bool healthy = s_ready && s_master_seen && !s_faulted && !expired && state_valid && uart_ready;
    s_faulted = s_faulted || expired || !state_valid || !uart_ready;
    bool faulted = s_faulted;
    taskEXIT_CRITICAL();
    if (faulted || !Monitor_ReportProgress(APP_MODULE_MODBUS, healthy))
        Monitor_ReportFatal(APP_MODULE_MODBUS);
}

/** @brief Poll任务启动时登记监督期限，等待总线应答仍由有界状态机负责。 */
bool mb_runtime_start_health(void)
{
    s_poll_owner = osThreadGetId();
    return s_ready && Monitor_RegisterHealth(APP_MODULE_MODBUS, MB_PROGRESS_MS, MB_PROGRESS_MS);
}

/** @brief 两个Modbus工作任务不能同步等待自己尚未处理的写请求。 */
bool mb_runtime_can_wait(void)
{
    return osThreadGetId() != s_poll_owner &&
           osThreadGetId() != hModbusMaster.myTaskModbusAHandle;
}

/** @brief 执行链失去应有通知时锁存故障，阻止新发送并停止监督喂狗。 */
void mb_runtime_fail(void)
{
    taskENTER_CRITICAL();
    s_faulted = true;
    taskEXIT_CRITICAL();
    Monitor_ReportFatal(APP_MODULE_MODBUS);
}
