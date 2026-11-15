/*
 * storage_runtime.c — 存储任务运行门控、结算与监督
 *
 * 业务变更持递归锁，暂停先关入口再等在途操作结束。
 * 结算、同步和跨任务序号分配使用固定值拷贝消息，Flash/FatFs由存储任务执行。
 */

#include "storage_runtime.h"
#include "storage_m2.h"
#include "storage_config.h"
#include "storage_ctrllog.h"
#include "storage_record.h"
#include "storage_seq.h"
#include "storage_upgrade_tf.h"
#include "upgrade_quiesce.h"
#include "app_upgrade.h"
#include "app_monitor.h"
#include "spi_flash_w25q128.h"
#include "sdcard_tf.h"
#include "ff_gen_drv.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "cmsis_os2.h"

#include <string.h>
#include <stdio.h>

enum { STORAGE_SETTLE = 1, STORAGE_SYNC, STORAGE_SEQUENCE, STORAGE_CONFIG };
typedef struct {
    uint32_t id;
    uint32_t deadline_ms;
    uint32_t action;
} storage_run_req_t;
typedef struct {
    uint32_t id;
    int32_t result;
    uint64_t sequence;
} storage_run_rsp_t;

static osMutexId_t s_gate;
static osMutexId_t s_client;
static StaticSemaphore_t s_gate_cb;
static StaticSemaphore_t s_client_cb;
static osMessageQueueId_t s_requests;
static osMessageQueueId_t s_responses;
static StaticQueue_t s_request_cb;
static StaticQueue_t s_response_cb;
_Alignas(8) static uint8_t s_request_mem[sizeof(storage_run_req_t)];
_Alignas(8) static uint8_t s_response_mem[sizeof(storage_run_rsp_t)];
static osThreadId_t s_owner;
static bool s_paused;
static bool s_ready;
static bool s_bootstrap;
static bool s_healthy;
static bool s_syncing;
static bool s_draining;
static bool s_faulted;
static uint32_t s_mutations;
static uint32_t s_deadline;
static uint32_t s_next_request;
static volatile uint32_t s_pending;
/* 原客户端锁和pending号保护值拷贝邮箱；超时客户端不能覆盖未完成请求。 */
static storage_config_message_t s_config_io;
extern Disk_drvTypeDef disk;
static Diskio_drvTypeDef s_disk;

/* HAL绝对期限均小于INT32_MAX ms。 */
static uint32_t runtime_remaining(uint32_t deadline)
{
    int32_t remaining = (int32_t)(deadline - HAL_GetTick());
    return remaining > 0 ? (uint32_t)remaining : 0U;
}

/* 毫秒向上换算RTOS tick，零用于立即尝试。 */
static uint32_t runtime_ticks(uint32_t ms)
{
    return (uint32_t)(((uint64_t)ms * osKernelGetTickFreq() + 999U) / 1000U);
}

/* 检查唯一介质所有者，禁止客户端同步等待自己的队列。 */
bool storage_runtime_is_owner(void)
{
    return s_owner != NULL && __get_IPSR() == 0U && osThreadGetId() == s_owner;
}

/* 仅反映准入，实际开始时仍须在锁内复查。 */
bool storage_mutation_allowed(void)
{
    taskENTER_CRITICAL();
    bool owner = storage_runtime_is_owner();
    bool continuing = owner && (s_mutations != 0U || s_draining);
    bool allowed = !s_faulted && (!s_paused || continuing) && (s_ready || (s_bootstrap && owner));
    taskEXIT_CRITICAL();
    return allowed && (continuing ? App_UpgradeCanCommit() : UpgradeQuiesce_AllowsMutation());
}

/* 递归锁覆盖业务操作及其嵌套序号/日志写入，暂停不与半个操作交错。 */
bool storage_mutation_begin(void)
{
    if (!storage_runtime_is_owner() || s_gate == NULL ||
        osMutexAcquire(s_gate, 0U) != osOK) return false;
    if (!storage_mutation_allowed()) {
        osMutexRelease(s_gate);
        return false;
    }
    s_mutations++;
    return true;
}

/* 只有成功begin才调用，锁深度和准入计数同时退出。 */
void storage_mutation_end(void)
{
    s_mutations--;
    osMutexRelease(s_gate);
}

/* 总期限不因分块或健康进展报告延长。 */
bool storage_runtime_expired(void)
{
    return !storage_runtime_is_owner() || runtime_remaining(s_deadline) == 0U;
}

/* 监督故障锁存，后续TF恢复或业务状态更新不能重新开放本次启动的写入。 */
static void runtime_fault(void)
{
    taskENTER_CRITICAL();
    s_faulted = true;
    s_ready = false;
    s_healthy = false;
    taskEXIT_CRITICAL();
    Monitor_ReportFatal(APP_MODULE_STORAGE);
}

/* 每次已完成I/O才报告进展；超期报告不能恢复看门狗资格。 */
void storage_runtime_checkpoint(void)
{
    if (!storage_runtime_is_owner()) return;
    if (!Monitor_ReportProgress(APP_MODULE_STORAGE, s_healthy) ||
        !Monitor_BeginWait(APP_MODULE_STORAGE, 6000U)) {
        runtime_fault();
    }
}

/* 业务读分成驱动块，读取失败不得用全FF代替。 */
HAL_StatusTypeDef storage_flash_read(uint32_t addr, uint8_t *data, uint32_t size)
{
    if (!storage_runtime_is_owner()) return HAL_BUSY;
    if (data == NULL || size == 0U || addr >= W25Q128_FLASH_SIZE_BYTES ||
        size > W25Q128_FLASH_SIZE_BYTES - addr) return HAL_ERROR;
    for (uint32_t done = 0U; done < size;) {
        uint32_t count = size - done;
        if (count > 256U) count = 256U;
        HAL_StatusTypeDef st = spi_flash_w25q128_read_until(addr + done, data + done, count, s_deadline);
        if (st != HAL_OK) {
            printf("[M3] Flash read failed addr=%06lX size=%lu status=%u\r\n",
                   (unsigned long)(addr + done), (unsigned long)count, (unsigned)st);
            return st;
        }
        storage_runtime_checkpoint();
        done += count;
    }
    return HAL_OK;
}

/* 只有已准入业务或显式结算同步可以写业务区；试运行始终拒绝。 */
static bool runtime_can_write(void)
{
    return storage_runtime_is_owner() && App_UpgradeCanCommit() &&
           (s_mutations != 0U || s_syncing);
}

/* 原有业务记录可大于256B，分块仍共用整个操作期限。 */
HAL_StatusTypeDef storage_flash_write(uint32_t addr, const uint8_t *data, uint32_t size)
{
    if (!runtime_can_write()) return HAL_BUSY;
    if (data == NULL || size == 0U || addr >= W25Q128_FLASH_SIZE_BYTES ||
        size > W25Q128_FLASH_SIZE_BYTES - addr) return HAL_ERROR;
    for (uint32_t done = 0U; done < size;) {
        uint32_t count = size - done;
        if (count > 256U) count = 256U;
        HAL_StatusTypeDef st = spi_flash_w25q128_write_until(addr + done, data + done, count, s_deadline);
        if (st != HAL_OK) return st;
        storage_runtime_checkpoint();
        done += count;
    }
    return HAL_OK;
}

/* 扇区/块擦除只由已准入操作执行。 */
HAL_StatusTypeDef storage_flash_erase(uint32_t addr)
{
    if (!runtime_can_write()) return HAL_BUSY;
    HAL_StatusTypeDef st = spi_flash_w25q128_erase_until(addr, false, s_deadline);
    if (st == HAL_OK) storage_runtime_checkpoint();
    return st;
}

/* 擦除一个业务回收块，使用当前轮次的原期限。 */
HAL_StatusTypeDef storage_flash_erase64(uint32_t addr)
{
    if (!runtime_can_write()) return HAL_BUSY;
    HAL_StatusTypeDef st = spi_flash_w25q128_erase_until(addr, true, s_deadline);
    if (st == HAL_OK) storage_runtime_checkpoint();
    return st;
}

/* 普通FatFs也采用同步扇区I/O，超时后不遗留DMA对栈缓冲的引用。 */
static DRESULT runtime_disk_read(BYTE lun, BYTE *data, DWORD sector, UINT count)
{
    (void)lun;
    if (!storage_runtime_is_owner() || count == 0U || sector > UINT32_MAX - count)
        return RES_PARERR;
    for (UINT i = 0U; i < count; i++) {
        if (sdcard_tf_read_until((uint32_t)sector + i, data + i * 512U, s_deadline) != HAL_OK)
            return RES_ERROR;
        storage_runtime_checkpoint();
    }
    return RES_OK;
}

/* 文件写入同样受试运行及业务准入保护，同步阶段只刷已有脏对象。 */
static DRESULT runtime_disk_write(BYTE lun, const BYTE *data, DWORD sector, UINT count)
{
    (void)lun;
    if (!runtime_can_write()) return RES_WRPRT;
    if (count == 0U || sector > UINT32_MAX - count) return RES_PARERR;
    for (UINT i = 0U; i < count; i++) {
        if (sdcard_tf_write_until((uint32_t)sector + i, data + i * 512U, s_deadline) != HAL_OK)
            return RES_ERROR;
        storage_runtime_checkpoint();
    }
    return RES_OK;
}

/* 只读检查所有已接纳对象，部分配置/控制回收和未消费请求都阻止安装。 */
static bool runtime_settled(void)
{
    return storage_m2_settled() && !storage_config_tx_active() &&
           !storage_ctrllog_is_busy() && !storage_upgrade_tf_is_open();
}

/* 唯一客户端锁和pending号隔离超时请求；不重发同步或分号操作。 */
static UpgradeStageResult runtime_exchange(uint32_t action, uint32_t timeout_ms, uint64_t *sequence,
                                            storage_config_message_t *config)
{
    if (s_client == NULL || s_requests == NULL || s_responses == NULL)
        return UPGRADE_STAGE_NOT_READY;
    if (storage_runtime_is_owner() || timeout_ms == 0U || timeout_ms > INT32_MAX)
        return UPGRADE_STAGE_MODE;
    uint32_t deadline = HAL_GetTick() + timeout_ms;
    if (osMutexAcquire(s_client, runtime_ticks(timeout_ms)) != osOK) return UPGRADE_STAGE_BUSY;
    UpgradeStageResult result = UPGRADE_STAGE_BUSY;
    if (s_pending == 0U && s_next_request != UINT32_MAX) {
        storage_run_req_t req = {++s_next_request, deadline, action};
        if (config != NULL) {
            /* BEGIN的归属在投递前确定，即使应答超时也能用原号取消。 */
            if (config->action == STORAGE_CONFIG_BEGIN || config->action == STORAGE_CONFIG_TF_OPEN)
                config->session_id = req.id;
            s_config_io = *config;
        }
        s_pending = req.id;
        uint32_t remaining = runtime_remaining(deadline);
        if (remaining == 0U || osMessageQueuePut(s_requests, &req, 0U, runtime_ticks(remaining)) != osOK) {
            s_pending = 0U;
            result = UPGRADE_STAGE_TIMEOUT;
        } else {
            storage_run_rsp_t rsp;
            result = UPGRADE_STAGE_TIMEOUT;
            while ((remaining = runtime_remaining(deadline)) != 0U) {
                if (osMessageQueueGet(s_responses, &rsp, NULL, runtime_ticks(remaining)) != osOK) break;
                if (rsp.id != req.id) continue;
                if (runtime_remaining(deadline) == 0U) break;
                result = (UpgradeStageResult)rsp.result;
                if (sequence != NULL && result == UPGRADE_STAGE_OK) *sequence = rsp.sequence;
                if (config != NULL && result == UPGRADE_STAGE_OK) *config = s_config_io;
                break;
            }
        }
    }
    osMutexRelease(s_client);
    return result;
}

/* 先关闭新变更，再等待已经进入的完整业务操作退出。 */
static UpgradeStageResult runtime_pause(void *context, uint32_t timeout_ms)
{
    (void)context;
    taskENTER_CRITICAL();
    s_paused = true;
    taskEXIT_CRITICAL();
    if (s_gate == NULL || timeout_ms == 0U ||
        osMutexAcquire(s_gate, runtime_ticks(timeout_ms)) != osOK)
        return UPGRADE_STAGE_TIMEOUT;
    taskENTER_CRITICAL();
    bool ready = s_ready && s_healthy && !s_faulted;
    taskEXIT_CRITICAL();
    osMutexRelease(s_gate);
    return ready ? UPGRADE_STAGE_OK : UPGRADE_STAGE_NOT_READY;
}

/* 结算和同步必须由存储所有者执行。 */
static UpgradeStageResult runtime_settle(void *context, uint32_t timeout_ms)
{
    (void)context;
    uint32_t started_ms = HAL_GetTick();
    for (;;) {
        uint32_t elapsed = (uint32_t)(HAL_GetTick() - started_ms);
        if (elapsed >= timeout_ms) return UPGRADE_STAGE_TIMEOUT;
        uint32_t remaining = timeout_ms - elapsed;
        UpgradeStageResult result = runtime_exchange(STORAGE_SETTLE, remaining, NULL, NULL);
        if (result != UPGRADE_STAGE_BUSY) return result;
        if (osDelay(1U) != osOK) return UPGRADE_STAGE_INTERNAL;
    }
}

/* 请求存储所有者刷新已有文件和环状态，失败不声明落盘完成。 */
static UpgradeStageResult runtime_sync(void *context, uint32_t timeout_ms)
{
    (void)context;
    return runtime_exchange(STORAGE_SYNC, timeout_ms, NULL, NULL);
}

/* 超时同步还未结束时不开放业务，后续显式恢复可再次检查。 */
static UpgradeStageResult runtime_resume(void *context, uint32_t timeout_ms)
{
    (void)context;
    if (s_pending != 0U) return UPGRADE_STAGE_BUSY;
    if (timeout_ms == 0U || osMutexAcquire(s_gate, runtime_ticks(timeout_ms)) != osOK)
        return UPGRADE_STAGE_TIMEOUT;
    UpgradeStageResult result = s_pending == 0U ? UPGRADE_STAGE_OK : UPGRADE_STAGE_BUSY;
    if (result == UPGRADE_STAGE_OK) {
        taskENTER_CRITICAL();
        s_paused = false;
        taskEXIT_CRITICAL();
    }
    osMutexRelease(s_gate);
    return result;
}

/* 由同步序号API复用既有协议，不在其他任务写Flash。 */
HAL_StatusTypeDef storage_runtime_alloc_seq(uint64_t *sequence)
{
    if (sequence == NULL) return HAL_ERROR;
    *sequence = 0U;
    if (!storage_mutation_allowed() || s_client == NULL) return HAL_BUSY;
    UpgradeStageResult result = runtime_exchange(STORAGE_SEQUENCE, 2000U, sequence, NULL);
    return result == UPGRADE_STAGE_OK ? HAL_OK :
           result == UPGRADE_STAGE_TIMEOUT ? HAL_TIMEOUT : HAL_BUSY;
}

/* Config只传消息值，Flash仍由Storage_Task独占；失败不复制不明输出。 */
HAL_StatusTypeDef storage_runtime_config(storage_config_message_t *message, uint32_t timeout_ms)
{
    if (message == NULL || timeout_ms == 0U || timeout_ms > 20000U) return HAL_ERROR;
    if (__get_IPSR() != 0U || osKernelGetState() != osKernelRunning) return HAL_BUSY;
    UpgradeStageResult result = runtime_exchange(STORAGE_CONFIG, timeout_ms, NULL, message);
    return result == UPGRADE_STAGE_OK ? HAL_OK :
           result == UPGRADE_STAGE_TIMEOUT ? HAL_TIMEOUT :
           result == UPGRADE_STAGE_STORAGE ? HAL_ERROR : HAL_BUSY;
}

/* 创建全部静态同步资源，不访问介质。 */
bool storage_runtime_init(void)
{
    static const osMutexAttr_t gate = {
        .name = "storage_gate", .attr_bits = osMutexRecursive | osMutexPrioInherit,
        .cb_mem = &s_gate_cb, .cb_size = sizeof(s_gate_cb)
    };
    static const osMutexAttr_t client = {
        .name = "storage_rpc", .attr_bits = osMutexPrioInherit,
        .cb_mem = &s_client_cb, .cb_size = sizeof(s_client_cb)
    };
    static const osMessageQueueAttr_t requests = {
        .name = "storage_run_req", .cb_mem = &s_request_cb, .cb_size = sizeof(s_request_cb),
        .mq_mem = s_request_mem, .mq_size = sizeof(s_request_mem)
    };
    static const osMessageQueueAttr_t responses = {
        .name = "storage_run_rsp", .cb_mem = &s_response_cb, .cb_size = sizeof(s_response_cb),
        .mq_mem = s_response_mem, .mq_size = sizeof(s_response_mem)
    };
    if (s_gate == NULL) s_gate = osMutexNew(&gate);
    if (s_client == NULL) s_client = osMutexNew(&client);
    if (s_requests == NULL) s_requests = osMessageQueueNew(1U, sizeof(storage_run_req_t), &requests);
    if (s_responses == NULL) s_responses = osMessageQueueNew(1U, sizeof(storage_run_rsp_t), &responses);
    return s_gate != NULL && s_client != NULL && s_requests != NULL && s_responses != NULL;
}

/* Task登记真实监督，原生成磁盘驱动只替换读写回调，保留初始化和状态函数。 */
bool storage_runtime_start(void)
{
    s_owner = osThreadGetId();
    s_bootstrap = true;
    if (!Monitor_RegisterHealth(APP_MODULE_STORAGE, 2000U, 30000U) ||
        !Monitor_ReportProgress(APP_MODULE_STORAGE, false)) return false;
    if (disk.drv[0] != NULL) {
        s_disk = *disk.drv[0];
        s_disk.disk_read = runtime_disk_read;
#if _USE_WRITE == 1
        s_disk.disk_write = runtime_disk_write;
#endif
        disk.drv[0] = &s_disk;
    }
    const UpgradeQuiesceParticipant participant = {
        runtime_pause, runtime_settle, runtime_sync, runtime_resume, NULL
    };
    /* 256分频扫描1MiB控制日志约需52s；恢复总限90s，每次I/O仍须报告进展。 */
    s_deadline = HAL_GetTick() + 90000U;
    return UpgradeQuiesce_Bind(UPGRADE_QUIESCE_STORAGE, &participant) == UPGRADE_STAGE_OK &&
           Monitor_BeginWait(APP_MODULE_STORAGE, 6000U);
}

/* 固定本阶段总期限；后续进展只影响监督，不延长该期限。 */
bool storage_runtime_wait(uint32_t timeout_ms)
{
    if (!storage_runtime_is_owner() || s_faulted || timeout_ms == 0U || timeout_ms > 25000U) return false;
    s_deadline = HAL_GetTick() + timeout_ms;
    return Monitor_BeginWait(APP_MODULE_STORAGE, timeout_ms + 1000U);
}

/* 核心恢复和只读自检完成后才允许业务准入。 */
void storage_runtime_finish(bool ready, bool healthy)
{
    taskENTER_CRITICAL();
    s_bootstrap = false;
    s_healthy = healthy && !s_faulted;
    s_ready = ready && !s_faulted;
    taskEXIT_CRITICAL();
    storage_runtime_report();
}

/* 一轮实际工作结束后退出等待，致命超期不能靠新报告恢复。 */
void storage_runtime_report(void)
{
    if (!Monitor_ReportProgress(APP_MODULE_STORAGE, s_healthy)) {
        runtime_fault();
    }
}

/* Storage_Task在业务门控之外处理，暂停期间仍能结算和回复客户端。 */
void storage_runtime_process(void)
{
    /* 只放行存储所有者收尾旧队列，不重新开放生产者或周期业务。 */
    HAL_StatusTypeDef m2_result = HAL_OK;
    if (s_paused && s_ready && s_healthy && !s_faulted) {
        s_draining = true;
        m2_result = storage_m2_process();
        s_draining = false;
    }
    storage_run_req_t req;
    storage_run_rsp_t rsp = {0};
    if (osMessageQueueGet(s_requests, &req, NULL, 0U) != osOK) return;
    rsp.id = req.id;
    rsp.result = UPGRADE_STAGE_TIMEOUT;
    /* Config在前段业务I/O已退出后开始独立操作，沿用请求的固定剩余期限。 */
    if (req.action == STORAGE_CONFIG) {
        uint32_t remaining = runtime_remaining(req.deadline_ms);
        storage_runtime_report();
        if (remaining != 0U && storage_runtime_wait(remaining)) {
            HAL_StatusTypeDef st = s_config_io.action >= STORAGE_CONFIG_TF_PROBE ?
                storage_config_tf(&s_config_io, req.deadline_ms) : storage_config_process(&s_config_io);
            rsp.result = st == HAL_OK ? UPGRADE_STAGE_OK :
                         st == HAL_TIMEOUT ? UPGRADE_STAGE_TIMEOUT :
                         st == HAL_BUSY ? UPGRADE_STAGE_BUSY : UPGRADE_STAGE_STORAGE;
        }
    } else {
        /* 既有序号/升级结算RPC不能延长本轮I/O及监督期限。 */
        if (runtime_remaining(req.deadline_ms) < runtime_remaining(s_deadline))
            s_deadline = req.deadline_ms;
        if (!storage_runtime_expired()) {
            if (req.action == STORAGE_SEQUENCE) {
                HAL_StatusTypeDef st = storage_seq_alloc(&rsp.sequence);
                rsp.result = st == HAL_OK ? UPGRADE_STAGE_OK :
                             st == HAL_TIMEOUT ? UPGRADE_STAGE_TIMEOUT : UPGRADE_STAGE_BUSY;
            } else if (!s_paused || !s_ready || !s_healthy) {
                rsp.result = UPGRADE_STAGE_NOT_READY;
            } else if (m2_result == HAL_ERROR || m2_result == HAL_TIMEOUT) {
                rsp.result = m2_result == HAL_TIMEOUT ? UPGRADE_STAGE_TIMEOUT : UPGRADE_STAGE_STORAGE;
            } else if (!runtime_settled()) {
                rsp.result = UPGRADE_STAGE_BUSY;
            } else if (req.action == STORAGE_SETTLE) {
                rsp.result = UPGRADE_STAGE_OK;
            } else if (req.action == STORAGE_SYNC) {
                s_syncing = true;
                HAL_StatusTypeDef st = storage_record_sync();
                s_syncing = false;
                rsp.result = st == HAL_OK ? UPGRADE_STAGE_OK :
                             st == HAL_TIMEOUT ? UPGRADE_STAGE_TIMEOUT : UPGRADE_STAGE_STORAGE;
            } else {
                rsp.result = UPGRADE_STAGE_INVALID;
            }
        }
    }
    if (s_pending == req.id) s_pending = 0U;
    if (osMessageQueuePut(s_responses, &rsp, 0U, 0U) != osOK) {
        storage_run_rsp_t stale;
        (void)osMessageQueueGet(s_responses, &stale, NULL, 0U);
        if (osMessageQueuePut(s_responses, &rsp, 0U, 0U) != osOK)
            printf("[M3] runtime response lost id=%lu\r\n", (unsigned long)rsp.id);
    }
}
