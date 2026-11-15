/*
 * storage_upgrade_tf.c — /firmware.gwp稳定只读租约
 *
 * 客户端只传值拷贝消息，Storage_Task持有FIL；返回前不再引用客户端缓冲。
 * FatFs文件锁阻止租约期间改写同一文件，拔卡/读失败使租约失效。
 */

#include "storage_upgrade_tf.h"
#include "upgrade_tf.h"
#include "storage_record.h"
#include "sdcard_tf.h"
#include "main.h"
#include "fatfs.h"
#include "ff_gen_drv.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "cmsis_os2.h"
#include "storage_runtime.h"

#include <string.h>
#include <stdio.h>

#define UPG_TF_CHUNK 256U
_Static_assert(_FS_LOCK >= 2, "Upgrade lease needs record and firmware file locks");
enum { TF_OPEN = 1, TF_READ, TF_CLOSE, TF_PROBE };

typedef struct {
    uint32_t req_id;
    uint32_t lease;
    uint32_t deadline_ms;
    uint32_t offset;
    uint16_t len;
    uint8_t type;
    uint8_t config_file;
} tf_req_t;

typedef struct {
    uint32_t req_id;
    uint32_t lease;
    uint32_t size;
    int32_t result;
    uint32_t detail;
    uint16_t actual_len;
    uint8_t type;
    uint8_t data[UPG_TF_CHUNK];
} tf_rsp_t;

static osMessageQueueId_t s_req_q;
static osMessageQueueId_t s_rsp_q;
static StaticQueue_t s_req_cb;
static StaticQueue_t s_rsp_cb;
_Alignas(8) static uint8_t s_req_mem[sizeof(tf_req_t)];
_Alignas(8) static uint8_t s_rsp_mem[sizeof(tf_rsp_t)];

/* 客户端由UpgradeTf的单导入操作串行调用，超时后仍保留lease供close结算。 */
static uint32_t s_next_req;
static uint32_t s_client_lease;
static bool s_bound;

/* 下面状态仅Storage_Task使用，文件及扇区缓冲均位于普通SRAM。 */
static FIL s_file;
static bool s_open;
static bool s_failed;
static uint32_t s_lease;
static uint32_t s_size;
static uint8_t s_config_file;
static uint32_t s_deadline;
_Alignas(8) static uint8_t s_sector[SDCARD_BLOCK_SIZE];
extern Disk_drvTypeDef disk;

/* 固定HAL期限，出队/换块不续期。 */
static uint32_t tf_remaining(uint32_t deadline_ms)
{
    int32_t remaining = (int32_t)(deadline_ms - HAL_GetTick());
    return remaining > 0 ? (uint32_t)remaining : 0U;
}

/* 毫秒向上取整为RTOS tick，调用方已拒绝零/过长期限。 */
static uint32_t tf_ticks(uint32_t ms)
{
    return (uint32_t)(((uint64_t)ms * osKernelGetTickFreq() + 999U) / 1000U);
}

/* 租约期间不允许FatFs隐式重新初始化或挂载另一个介质。 */
static DSTATUS tf_disk_status(BYTE lun)
{
    (void)lun;
    return storage_record_is_ready() &&
           HAL_GPIO_ReadPin(SD_CARD_DET_GPIO_Port, SD_CARD_DET_Pin) == GPIO_PIN_RESET
         ? 0U : STA_NOINIT;
}

/* 仅替换本次只读FatFs调用的扇区读取，所有扇区共用原请求期限。 */
static DRESULT tf_disk_read(BYTE lun, BYTE *buf, DWORD sector, UINT count)
{
    if (tf_disk_status(lun) != 0U || count == 0U || sector > UINT32_MAX - count)
        return RES_NOTRDY;
    for (UINT i = 0U; i < count; i++) {
        HAL_StatusTypeDef st = sdcard_tf_read_until((uint32_t)sector + i, s_sector, s_deadline);
        if (st != HAL_OK) return RES_ERROR;
        memcpy(buf + i * SDCARD_BLOCK_SIZE, s_sector, SDCARD_BLOCK_SIZE);
    }
    return RES_OK;
}

/* 文件服务只读，意外写请求不能落入普通无期限驱动。 */
static DRESULT tf_disk_write(BYTE lun, const BYTE *buf, DWORD sector, UINT count)
{
    (void)lun; (void)buf; (void)sector; (void)count;
    return RES_WRPRT;
}

/* 在单一存储所有者内临时替换磁盘读回调；返回前恢复生成驱动，不改生成代码。 */
static FRESULT tf_file_io(const tf_req_t *req, uint8_t *data, UINT *actual, uint32_t *files)
{
    if (disk.drv[0] == NULL || disk.is_initialized[0] == 0U) return FR_NOT_READY;
    const Diskio_drvTypeDef *original = disk.drv[0];
    Diskio_drvTypeDef bounded = *original;
    bounded.disk_initialize = tf_disk_status;
    bounded.disk_status = tf_disk_status;
    bounded.disk_read = tf_disk_read;
#if _USE_WRITE == 1
    bounded.disk_write = tf_disk_write;
#endif
    disk.drv[0] = &bounded;

    FRESULT fr;
    if (req->type == TF_PROBE) {
        *files = 0U;
        FILINFO info;
        fr = FR_OK;
        const char *paths[2] = {STORAGE_TF_CONFIG_PATH, UPGRADE_TF_PATH};
        for (uint32_t i = 0U; i < 2U; i++) {
            FRESULT found = f_stat(paths[i], &info);
            if (found == FR_NO_FILE || found == FR_NO_PATH) continue;
            if (found != FR_OK) { fr = found; break; }
            if ((info.fattrib & AM_DIR) != 0U) { fr = FR_INVALID_OBJECT; break; }
            *files |= 1UL << i;
        }
    } else if (req->type == TF_OPEN) {
        fr = f_open(&s_file, req->config_file ? STORAGE_TF_CONFIG_PATH : UPGRADE_TF_PATH, FA_READ);
    } else if (req->type == TF_CLOSE) {
        fr = f_close(&s_file);
    } else {
        fr = f_lseek(&s_file, req->offset);
        if (fr == FR_OK && f_tell(&s_file) != req->offset) fr = FR_INT_ERR;
        if (fr == FR_OK) fr = f_read(&s_file, data, req->len, actual);
    }
    disk.drv[0] = original;
    return fr;
}

/* 执行单个文件请求；失败保留租约供幂等关闭，不允许复用旧FIL。 */
static void tf_handle(const tf_req_t *req, tf_rsp_t *rsp)
{
    memset(rsp, 0, sizeof(*rsp));
    rsp->req_id = req->req_id;
    rsp->lease = req->lease;
    rsp->type = req->type;
    rsp->result = UPGRADE_STAGE_INVALID;
    s_deadline = req->deadline_ms;
    if ((req->lease == 0U && req->type != TF_PROBE) || req->req_id == 0U ||
        req->type < TF_OPEN || req->type > TF_PROBE || req->config_file > 1U) return;
    if (tf_remaining(s_deadline) == 0U) {
        rsp->result = UPGRADE_STAGE_TIMEOUT;
        return;
    }
    if (req->type == TF_PROBE) {
        if (s_open || !storage_mutation_allowed()) { rsp->result = UPGRADE_STAGE_BUSY; return; }
        UINT unused = 0U;
        FRESULT fr = tf_file_io(req, NULL, &unused, &rsp->size);
        rsp->detail = (uint32_t)fr;
        rsp->result = tf_remaining(s_deadline) == 0U ? UPGRADE_STAGE_TIMEOUT :
                      fr == FR_OK ? UPGRADE_STAGE_OK : UPGRADE_STAGE_STORAGE;
        return;
    }
    if (req->type == TF_CLOSE && !s_open) {
        rsp->result = UPGRADE_STAGE_OK;
        return;
    }
    if (req->type == TF_OPEN) {
        if (!storage_mutation_allowed()) {
            rsp->result = UPGRADE_STAGE_MODE;
            return;
        }
        if (s_open) {
            rsp->result = UPGRADE_STAGE_BUSY;
            return;
        }
        if (!storage_record_is_ready() || tf_disk_status(0U) != 0U) {
            rsp->result = UPGRADE_STAGE_NOT_READY;
            return;
        }
    } else if (!s_open || req->lease != s_lease || req->config_file != s_config_file) {
        rsp->result = UPGRADE_STAGE_MODE;
        return;
    } else if (req->type == TF_READ &&
               (s_failed || tf_disk_status(0U) != 0U || f_size(&s_file) != s_size ||
                req->len == 0U || req->len > UPG_TF_CHUNK ||
                req->offset > s_size || req->len > s_size - req->offset)) {
        rsp->result = UPGRADE_STAGE_MODE;
        return;
    }

    UINT actual = 0U;
    FRESULT fr = tf_file_io(req, rsp->data, &actual, &rsp->size);
    rsp->detail = (uint32_t)fr;
    if (req->type == TF_OPEN && fr == FR_OK) {
        s_open = true;
        s_failed = false;
        s_lease = req->lease;
        s_config_file = req->config_file;
        s_size = (uint32_t)f_size(&s_file);
        rsp->size = s_size;
    } else if (req->type == TF_CLOSE && fr == FR_OK) {
        s_open = false;
        s_failed = false;
    }
    if (tf_remaining(s_deadline) == 0U) {
        if (s_open) s_failed = true;
        rsp->result = UPGRADE_STAGE_TIMEOUT;
    } else if (fr != FR_OK) {
        if (s_open) s_failed = true;
        rsp->result = UPGRADE_STAGE_STORAGE;
        printf("[UPGTF] I/O failed op=%u offset=%lu fr=%u\r\n",
               (unsigned)req->type, (unsigned long)req->offset, (unsigned)fr);
    } else if (req->type == TF_READ && actual != req->len) {
        s_failed = true;
        rsp->result = UPGRADE_STAGE_INTEGRITY;
    } else {
        rsp->actual_len = (uint16_t)actual;
        rsp->result = UPGRADE_STAGE_OK;
    }
}

/* Config复用原运行RPC，配置和固件租约即使同号也不能互相读取或关闭。 */
HAL_StatusTypeDef storage_config_tf(storage_config_message_t *message, uint32_t deadline_ms)
{
    if (!storage_runtime_is_owner() || message == NULL || message->len > UPG_TF_CHUNK)
        return HAL_ERROR;
    uint8_t type = message->action == STORAGE_CONFIG_TF_OPEN ? TF_OPEN :
                   message->action == STORAGE_CONFIG_TF_READ ? TF_READ :
                   message->action == STORAGE_CONFIG_TF_CLOSE ? TF_CLOSE :
                   message->action == STORAGE_CONFIG_TF_PROBE ? TF_PROBE : 0U;
    if (type == 0U) return HAL_ERROR;
    tf_req_t req = {.req_id = 1U, .lease = message->session_id, .deadline_ms = deadline_ms,
                    .offset = message->offset, .len = (uint16_t)message->len, .type = type, .config_file = 1U};
    tf_rsp_t rsp;
    tf_handle(&req, &rsp);
    if (rsp.result != UPGRADE_STAGE_OK)
        return rsp.result == UPGRADE_STAGE_TIMEOUT ? HAL_TIMEOUT :
               rsp.result == UPGRADE_STAGE_BUSY || rsp.result == UPGRADE_STAGE_MODE ? HAL_BUSY : HAL_ERROR;
    if (type == TF_OPEN || type == TF_PROBE) message->total_len = rsp.size;
    if (type == TF_READ) {
        if (rsp.actual_len != message->len) return HAL_ERROR;
        memcpy(message->data, rsp.data, rsp.actual_len);
    }
    return HAL_OK;
}

/* 同步等待对应应答，迟到消息丢弃；队列中始终没有客户端指针。 */
static UpgradeStageResult tf_exchange(tf_req_t *req, tf_rsp_t *rsp)
{
    if (s_req_q == NULL || s_rsp_q == NULL) return UPGRADE_STAGE_NOT_READY;
    if (s_next_req == UINT32_MAX) return UPGRADE_STAGE_MODE;
    req->req_id = ++s_next_req;
    uint32_t remaining = tf_remaining(req->deadline_ms);
    if (remaining == 0U) return UPGRADE_STAGE_TIMEOUT;
    osStatus_t st = osMessageQueuePut(s_req_q, req, 0U, tf_ticks(remaining));
    if (st != osOK) return UPGRADE_STAGE_TIMEOUT;
    while ((remaining = tf_remaining(req->deadline_ms)) != 0U) {
        st = osMessageQueueGet(s_rsp_q, rsp, NULL, tf_ticks(remaining));
        if (st != osOK) return UPGRADE_STAGE_TIMEOUT;
        if (tf_remaining(req->deadline_ms) == 0U) return UPGRADE_STAGE_TIMEOUT;
        if (rsp->req_id == req->req_id && rsp->lease == req->lease && rsp->type == req->type)
            return (UpgradeStageResult)rsp->result;
    }
    return UPGRADE_STAGE_TIMEOUT;
}

/* 固定路径打开；失败也保留客户端租约号，后续close可结算迟到的open。 */
static UpgradeStageResult tf_open(void *context, const char *path, uint32_t *size, uint32_t timeout_ms)
{
    (void)context;
    if (size == NULL) return UPGRADE_STAGE_INVALID;
    *size = 0U;
    if (path == NULL || strcmp(path, UPGRADE_TF_PATH) != 0 ||
        timeout_ms == 0U || timeout_ms > INT32_MAX) return UPGRADE_STAGE_INVALID;
    if (s_client_lease != 0U || s_next_req == UINT32_MAX) return UPGRADE_STAGE_BUSY;
    s_client_lease = ++s_next_req;
    tf_req_t req = {.lease = s_client_lease, .type = TF_OPEN,
                    .deadline_ms = HAL_GetTick() + timeout_ms};
    tf_rsp_t rsp;
    UpgradeStageResult st = tf_exchange(&req, &rsp);
    if (st == UPGRADE_STAGE_OK) *size = rsp.size;
    return st;
}

/* 读回完整填充才交给上层，分块不延长同一次read的期限。 */
static UpgradeStageResult tf_read(void *context, uint32_t offset, uint8_t *data,
                                  uint32_t size, uint32_t timeout_ms)
{
    (void)context;
    if (data == NULL || size == 0U || size > UPGRADE_STAGE_MAX_CHUNK_SIZE ||
        offset > UINT32_MAX - size || timeout_ms == 0U || timeout_ms > INT32_MAX)
        return UPGRADE_STAGE_INVALID;
    if (s_client_lease == 0U) return UPGRADE_STAGE_MODE;
    uint32_t deadline = HAL_GetTick() + timeout_ms;
    for (uint32_t done = 0U; done < size;) {
        uint32_t count = size - done;
        if (count > UPG_TF_CHUNK) count = UPG_TF_CHUNK;
        tf_req_t req = {.lease = s_client_lease, .type = TF_READ, .deadline_ms = deadline,
                        .offset = offset + done, .len = (uint16_t)count};
        tf_rsp_t rsp;
        UpgradeStageResult st = tf_exchange(&req, &rsp);
        if (st != UPGRADE_STAGE_OK) return st;
        if (rsp.actual_len != count) return UPGRADE_STAGE_INTEGRITY;
        memcpy(data + done, rsp.data, count);
        done += count;
    }
    return UPGRADE_STAGE_OK;
}

/* 关闭明确成功才释放客户端租约，旧close不得关闭新租约。 */
static UpgradeStageResult tf_close(void *context, uint32_t timeout_ms)
{
    (void)context;
    if (s_client_lease == 0U) return UPGRADE_STAGE_OK;
    if (timeout_ms == 0U || timeout_ms > INT32_MAX) return UPGRADE_STAGE_INVALID;
    tf_req_t req = {.lease = s_client_lease, .type = TF_CLOSE,
                    .deadline_ms = HAL_GetTick() + timeout_ms};
    tf_rsp_t rsp;
    UpgradeStageResult st = tf_exchange(&req, &rsp);
    if (st == UPGRADE_STAGE_OK) s_client_lease = 0U;
    return st;
}

/* 创建真实值拷贝队列，不创建新任务，也不在初始化期间访问FatFs。 */
bool storage_upgrade_tf_init(void)
{
    static const osMessageQueueAttr_t req_attr = {
        .name = "upg_tf_req", .cb_mem = &s_req_cb, .cb_size = sizeof(s_req_cb),
        .mq_mem = s_req_mem, .mq_size = sizeof(s_req_mem)
    };
    static const osMessageQueueAttr_t rsp_attr = {
        .name = "upg_tf_rsp", .cb_mem = &s_rsp_cb, .cb_size = sizeof(s_rsp_cb),
        .mq_mem = s_rsp_mem, .mq_size = sizeof(s_rsp_mem)
    };
    if (s_req_q == NULL) s_req_q = osMessageQueueNew(1U, sizeof(tf_req_t), &req_attr);
    if (s_rsp_q == NULL) s_rsp_q = osMessageQueueNew(1U, sizeof(tf_rsp_t), &rsp_attr);
    return s_req_q != NULL && s_rsp_q != NULL;
}

/* 文件服务已能处理真实请求后才绑定；卡缺失由open按降级结果报告。 */
void storage_upgrade_tf_bind(void)
{
    if (s_bound || s_req_q == NULL || s_rsp_q == NULL) return;
    const UpgradeTfSource source = {tf_open, tf_read, tf_close, NULL};
    UpgradeStageResult st = UpgradeTf_Bind(&source);
    if (st == UPGRADE_STAGE_OK) s_bound = true;
    else printf("[UPGTF] bind failed result=%d\r\n", (int)st);
}

/* 值拷贝应答不引用FIL或DMA缓冲，深度1满时替换迟到应答。 */
void storage_upgrade_tf_process(void)
{
    if (s_req_q == NULL || s_rsp_q == NULL) return;
    tf_req_t req;
    tf_rsp_t rsp;
    if (osMessageQueueGet(s_req_q, &req, NULL, 0U) != osOK) return;
    tf_handle(&req, &rsp);
    if (osMessageQueuePut(s_rsp_q, &rsp, 0U, 0U) == osOK) return;
    tf_rsp_t stale;
    (void)osMessageQueueGet(s_rsp_q, &stale, NULL, 0U);
    if (osMessageQueuePut(s_rsp_q, &rsp, 0U, 0U) != osOK)
        printf("[UPGTF] response LOST req=%lu\r\n", (unsigned long)rsp.req_id);
}

/* 插拔后禁止继续读取；只有卸载/重挂使文件锁失效后才丢弃旧FIL。 */
void storage_upgrade_tf_invalidate(bool unmounted)
{
    if (unmounted) s_open = false;
    s_failed = true;
}

/* 仅存储所有者查询，打开或关闭失败的租约都仍占用文件。 */
bool storage_upgrade_tf_is_open(void)
{
    return s_open;
}
