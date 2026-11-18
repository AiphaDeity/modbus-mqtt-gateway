/*
 * storage_upgrade.c — 升级包槽与持久事务的存储端
 *
 * Storage_Task独占执行；包认证及启动决策由升级服务/Boot完成。
 * 本模块只允许候选槽数据、事务高水位和受约束的READY提交。
 */

#include "storage_upgrade.h"
#include "upgrade_state.h"
#include "upgrade_transaction.h"
#include "spi_flash_w25q128.h"
#include "flash_partition.h"
#include "cmsis_os2.h"
#include "storage_runtime.h"

#include <string.h>

#define UPG_PKG_MIN_LEN 216U
#define UPG_PKG_MAX_LEN (448U * 1024U + 208U)
#define UPG_BLOCK_SIZE 65536U

typedef struct {
    bool active;
    bool failed;
    bool commit_attempted;
    uint8_t slot;
    uint32_t session;
    uint32_t total;
    uint32_t written;
    uint64_t tx_id;
    UpgradeStateRecord baseline;
} upg_session_t;

static bool s_ready;
static osThreadId_t s_owner;
static uint32_t s_next_session;
static uint64_t s_reserved_tx;
static upg_session_t s_tx;

/* 单请求执行上下文，供shared存储回调传递同一总期限及首个I/O错误。 */
static uint32_t s_deadline;
static UpgradeStorageResult s_io_result;
static uint32_t s_detail;

/* 期限间隔由上层限制在INT32_MAX ms内，HAL tick回绕时仍可比较。 */
static bool upg_expired(void)
{
    return (int32_t)(s_deadline - HAL_GetTick()) <= 0;
}

/* 映射HAL结果，底层原值单独保存在detail，不直接作为协议结果。 */
static bool upg_io(HAL_StatusTypeDef st)
{
    if (st == HAL_OK) {
        storage_runtime_checkpoint();
        return true;
    }
    if (s_io_result == UPGRADE_STORAGE_OK) {
        s_io_result = st == HAL_TIMEOUT ? UPGRADE_STORAGE_TIMEOUT : UPGRADE_STORAGE_IO_ERROR;
        s_detail = (uint32_t)st;
    }
    return false;
}

/* shared日志只读回调：offset相对UPGLOG，拒绝短读和元数据区外访问。 */
static bool upg_read_metadata(void *context, uint32_t offset, uint8_t *data, uint32_t size)
{
    (void)context;
    if (data == NULL || size == 0U || size > UPGRADE_STORAGE_CHUNK_SIZE ||
        offset >= UPGRADE_STATE_METADATA_SIZE || size > UPGRADE_STATE_METADATA_SIZE - offset)
        return false;
    return upg_io(spi_flash_w25q128_read_until(FLASH_PART_UPGLOG_BASE + offset,
                                               data, size, s_deadline));
}

/* shared事务擦写只能落在高水位双扇区。 */
static bool upg_erase_transaction(void *context, uint32_t offset)
{
    (void)context;
    if (offset != UPGRADE_STATE_TRANSACTION_OFFSET &&
        offset != UPGRADE_STATE_TRANSACTION_OFFSET + UPGRADE_STATE_SECTOR_SIZE)
        return false;
    return upg_io(spi_flash_w25q128_erase_until(FLASH_PART_UPGLOG_BASE + offset,
                                                false, s_deadline));
}

/* 写入事务正文或提交标记，跨页由有界驱动分段。 */
static bool upg_program_transaction(void *context, uint32_t offset,
                                     const uint8_t *data, uint32_t size)
{
    (void)context;
    uint32_t end = UPGRADE_STATE_TRANSACTION_OFFSET + 2U * UPGRADE_STATE_SECTOR_SIZE;
    if (offset < UPGRADE_STATE_TRANSACTION_OFFSET || offset >= end ||
        size == 0U || size > UPGRADE_STORAGE_CHUNK_SIZE || size > end - offset)
        return false;
    return upg_io(spi_flash_w25q128_write_until(FLASH_PART_UPGLOG_BASE + offset,
                                                data, size, s_deadline));
}

static const UpgradeTransactionStorage s_transaction = {
    upg_read_metadata, upg_erase_transaction, upg_program_transaction, NULL
};

/* shared枚举与队列枚举分别映射，优先保留真实I/O失败。 */
static UpgradeStorageResult upg_state_result(UpgradeStateResult result)
{
    if (s_io_result != UPGRADE_STORAGE_OK) return s_io_result;
    switch (result) {
    case UPGRADE_STATE_OK: return UPGRADE_STORAGE_OK;
    case UPGRADE_STATE_EMPTY: return UPGRADE_STORAGE_NOT_READY;
    case UPGRADE_STATE_IO_ERROR: return UPGRADE_STORAGE_IO_ERROR;
    case UPGRADE_STATE_CORRUPT: return UPGRADE_STORAGE_CORRUPT;
    case UPGRADE_STATE_UNSUPPORTED: return UPGRADE_STORAGE_UNSUPPORTED;
    default: return UPGRADE_STORAGE_INVALID;
    }
}

/* 线协议0/A、1/B转换为实际Flash槽基址。 */
static uint32_t upg_slot_base(uint8_t slot)
{
    return slot == 0U ? FLASH_PART_FW_A_BASE : FLASH_PART_FW_B_BASE;
}

/* 比较完整持久身份，不能只看phase或CRC。 */
static bool upg_same_record(const UpgradeStateRecord *a, const UpgradeStateRecord *b)
{
    return a->sequence == b->sequence && a->phase == b->phase &&
           a->tx_id == b->tx_id && a->trial_attempts == b->trial_attempts &&
           a->operation == b->operation &&
           UpgradeState_PackageEqual(&a->confirmed, &b->confirmed) &&
           UpgradeState_PackageEqual(&a->candidate, &b->candidate);
}

/* 执行时复核调用方预期视图；读取错误/恢复屏障不能当空设备。 */
static UpgradeStorageResult upg_load_expected(const UpgradeStorageRequest *req,
                                              UpgradeStateProtection *view)
{
    UpgradeStateResult result = UpgradeState_ReadProtection(upg_read_metadata, NULL, view);
    UpgradeStorageResult st = upg_state_result(result);
    if (st != UPGRADE_STORAGE_OK) return st;
    if (req->expected_normal_result != (uint32_t)view->normal_result ||
        req->expected_recovery_result != (uint32_t)view->recovery_result ||
        req->expected_normal_sequence != view->normal.sequence ||
        req->expected_recovery_sequence != view->recovery.sequence)
        return UPGRADE_STORAGE_CONFLICT;
    return UPGRADE_STORAGE_OK;
}

/* 先提交并复读高水位，再交付非零事务号；失败不能回收可能消耗的ID。 */
static UpgradeStorageResult upg_reserve(UpgradeStorageResponse *rsp)
{
    if (s_tx.active) return UPGRADE_STORAGE_BUSY;
    s_reserved_tx = 0U;
    UpgradeStateProtection view;
    UpgradeStorageResult st = upg_state_result(
        UpgradeState_ReadProtection(upg_read_metadata, NULL, &view));
    if (st != UPGRADE_STORAGE_OK) return st;
    if (view.writable_slots == 0U) return UPGRADE_STORAGE_CONFLICT;
    uint64_t tx_id = 0U;
    UpgradeStateResult result = UpgradeTransaction_Reserve(&s_transaction, &tx_id);
    st = upg_state_result(result);
    if (st == UPGRADE_STORAGE_OK) {
        s_reserved_tx = tx_id;
        rsp->tx_id = tx_id;
    }
    return st;
}

/* 独占并复核候选后擦槽；失败保留租约，供已知会话或tx_id取消。 */
static UpgradeStorageResult upg_begin(const UpgradeStorageRequest *req, UpgradeStorageResponse *rsp)
{
    if (s_tx.active) return UPGRADE_STORAGE_BUSY;
    if (req->session != 0U || req->slot > 1U || req->pkg_len < UPG_PKG_MIN_LEN ||
        req->pkg_len > UPG_PKG_MAX_LEN || req->tx_id == 0U ||
        req->tx_id != s_reserved_tx || s_next_session == UINT32_MAX)
        return UPGRADE_STORAGE_INVALID;

    UpgradeStateProtection view;
    UpgradeStorageResult st = upg_load_expected(req, &view);
    if (st != UPGRADE_STORAGE_OK) return st;
    // 上层选槽后，持久状态可能已有变化；擦写前重新确认该槽仍可写且没有独立恢复屏障。
    if (view.normal_result != UPGRADE_STATE_OK || view.recovery_result != UPGRADE_STATE_EMPTY ||
        view.normal.phase != UPGRADE_STATE_CONFIRMED ||
        view.normal.sequence == UINT32_MAX ||
        (view.writable_slots & (1U << req->slot)) == 0U || req->tx_id <= view.normal.tx_id)
        return UPGRADE_STORAGE_CONFLICT;

    memset(&s_tx, 0, sizeof(s_tx));
    s_tx.active = true;
    s_tx.session = ++s_next_session;
    s_tx.slot = req->slot;
    s_tx.total = req->pkg_len;
    s_tx.tx_id = req->tx_id;
    s_tx.baseline = view.normal;
    s_reserved_tx = 0U;

    // 租约先绑定到已核准的候选槽，后续块不能重新挑槽；擦槽失败也保留失败租约供结算。
    uint32_t base = upg_slot_base(req->slot);
    for (uint32_t offset = 0U; offset < FLASH_PART_FW_A_END - FLASH_PART_FW_A_BASE;
         offset += UPG_BLOCK_SIZE) {
        if (!upg_io(spi_flash_w25q128_erase_until(base + offset, true, s_deadline))) {
            s_tx.failed = true;
            return s_io_result;
        }
    }
    rsp->session = s_tx.session;
    return UPGRADE_STORAGE_OK;
}

/* 候选访问同时核对session和tx_id，旧事务不能操作新租约。 */
static bool upg_matches(const UpgradeStorageRequest *req)
{
    return s_tx.active && req->session == s_tx.session && req->tx_id == s_tx.tx_id;
}

/*
 * 执行 CHUNK 请求：按租约和连续偏移写候选槽，将 .gwp 作为不透明字节处理，不验签或解密。
 * 写入和完整读回比较都成功才推进 written；介质读写或比较失败会锁存故障，拒绝后续块追加。
 */
static UpgradeStorageResult upg_chunk(const UpgradeStorageRequest *req)
{
    if (!upg_matches(req)) return UPGRADE_STORAGE_CONFLICT;
    if (s_tx.failed || s_tx.commit_attempted) return UPGRADE_STORAGE_CONFLICT;
    if (req->len == 0U || req->len > UPGRADE_STORAGE_CHUNK_SIZE ||
        req->offset != s_tx.written || req->offset > s_tx.total ||
        req->len > s_tx.total - req->offset) return UPGRADE_STORAGE_INVALID;

    // 先核对会话和边界，再将包内偏移换算为 Begin 已锁定槽的实际 Flash 地址。
    uint8_t check[UPGRADE_STORAGE_CHUNK_SIZE];
    uint32_t address = upg_slot_base(s_tx.slot) + req->offset;
    if (!upg_io(spi_flash_w25q128_write_until(address, req->data, req->len, s_deadline)) ||
        !upg_io(spi_flash_w25q128_read_until(address, check, req->len, s_deadline))) {
        s_tx.failed = true;
        return s_io_result;
    }
    // 本层比较单条 RPC 的实际读回；返回后 Stage 还会对原上层块再次独立读回比较。
    if (memcmp(check, req->data, req->len) != 0) {
        s_tx.failed = true;
        return UPGRADE_STORAGE_CORRUPT;
    }
    s_tx.written += req->len;
    return UPGRADE_STORAGE_OK;
}

/* 只允许读取当前候选已写范围，或普通CONFIRMED所引用的可信源范围。 */
static UpgradeStorageResult upg_read_package(const UpgradeStorageRequest *req,
                                              UpgradeStorageResponse *rsp)
{
    if (req->slot > 1U || req->len == 0U || req->len > UPGRADE_STORAGE_CHUNK_SIZE)
        return UPGRADE_STORAGE_INVALID;
    uint32_t limit;
    if (req->session == 0U && req->tx_id == 0U) {
        UpgradeStateProtection view;
        UpgradeStorageResult st = upg_state_result(
            UpgradeState_ReadProtection(upg_read_metadata, NULL, &view));
        if (st != UPGRADE_STORAGE_OK) return st;
        if (view.normal_result != UPGRADE_STATE_OK || view.recovery_result != UPGRADE_STATE_EMPTY ||
            view.normal.phase != UPGRADE_STATE_CONFIRMED ||
            view.normal.confirmed.slot != (uint32_t)req->slot + 1U)
            return UPGRADE_STORAGE_CONFLICT;
        limit = view.normal.confirmed.size;
    } else {
        if (!upg_matches(req) || req->slot != s_tx.slot) return UPGRADE_STORAGE_CONFLICT;
        limit = s_tx.written;
    }
    if (req->offset > limit || req->len > limit - req->offset) return UPGRADE_STORAGE_INVALID;
    if (!upg_io(spi_flash_w25q128_read_until(upg_slot_base(req->slot) + req->offset,
                                            rsp->data, req->len, s_deadline)))
        return s_io_result;
    rsp->actual_len = req->len;
    return UPGRADE_STORAGE_OK;
}

/*
 * 执行 COMMIT_READY 请求：只接受本租约对应的完整 READY/INSTALL 描述，保留已确认包描述。
 * 包认证由上层负责，本层核对会话、完整接收量和持久基线，不在此安装片内 App。
 * 保留当前有效日志，在另一扇区写正文和独立提交标记，再重新加载核对；失败不得盲目重发。
 */
static UpgradeStorageResult upg_commit(const UpgradeStorageRequest *req)
{
    if (!upg_matches(req) || req->slot != s_tx.slot || s_tx.failed)
        return UPGRADE_STORAGE_CONFLICT;
    // 1. 完整包已写入才可提交；请求必须是本租约的下一条 READY，且仍引用原已确认包。
    UpgradeStateRecord next;
    if (req->len != UPGRADE_STATE_RECORD_SIZE || s_tx.written != s_tx.total ||
        !UpgradeState_Decode(req->data, &next) || next.phase != UPGRADE_STATE_READY ||
        next.operation != UPGRADE_OPERATION_INSTALL || next.trial_attempts != 0U ||
        next.tx_id != s_tx.tx_id || next.candidate.slot != (uint32_t)s_tx.slot + 1U ||
        next.candidate.size != s_tx.total ||
        !UpgradeState_PackageEqual(&next.confirmed, &s_tx.baseline.confirmed) ||
        s_tx.baseline.sequence == UINT32_MAX || next.sequence != s_tx.baseline.sequence + 1U ||
        !UpgradeState_TransitionValid(&s_tx.baseline, &next))
        return UPGRADE_STORAGE_INVALID;

    UpgradeStateProtection view;
    UpgradeStorageResult st = upg_state_result(
        UpgradeState_ReadProtection(upg_read_metadata, NULL, &view));
    if (st != UPGRADE_STORAGE_OK) return st;
    // 2. 同一 READY 已完整落盘时只确认已有结果；否则须复核原基线，不能用过期快照覆盖新状态。
    if (view.recovery_result == UPGRADE_STATE_EMPTY && upg_same_record(&view.normal, &next))
        return UPGRADE_STORAGE_OK;
    if (s_tx.commit_attempted) return UPGRADE_STORAGE_CONFLICT;
    st = upg_load_expected(req, &view);
    if (st != UPGRADE_STORAGE_OK) return st;
    if (!upg_same_record(&view.normal, &s_tx.baseline) ||
        view.recovery_result != UPGRADE_STATE_EMPTY) return UPGRADE_STORAGE_CONFLICT;

    uint32_t selected;
    UpgradeStateRecord old;
    st = upg_state_result(UpgradeState_Load(upg_read_metadata, NULL, &old, &selected));
    if (st != UPGRADE_STORAGE_OK) return st;
    if (!upg_same_record(&old, &s_tx.baseline)) return UPGRADE_STORAGE_CONFLICT;
    // 3. 切换的是日志副本 0/1，不是加密包槽；当前有效副本和受保护旧包均保持不动。
    uint32_t address = FLASH_PART_UPGLOG_BASE + (selected ^ 1U) * UPGRADE_STATE_SECTOR_SIZE;
    const uint8_t marker[UPGRADE_STATE_COMMIT_SIZE] = UPGRADE_STATE_NORMAL_COMMIT;
    // 4. 先锁存一次提交尝试，再擦目标、写正文、最后写 CMIT；后续错误可能发生在已提交之后。
    s_tx.commit_attempted = true;
    if (!upg_io(spi_flash_w25q128_erase_until(address, false, s_deadline)) ||
        !upg_io(spi_flash_w25q128_write_until(address, req->data, req->len, s_deadline)) ||
        !upg_io(spi_flash_w25q128_write_until(address + UPGRADE_STATE_COMMIT_OFFSET,
                                               marker, sizeof(marker), s_deadline)))
        return s_io_result;
    // 5. 重新经过提交标记、CRC 和序号选择，再比较完整身份；只有实际读回一致才应答成功。
    st = upg_state_result(UpgradeState_Load(upg_read_metadata, NULL, &old, &selected));
    if (st != UPGRADE_STORAGE_OK) return st;
    return upg_same_record(&old, &next) ? UPGRADE_STORAGE_OK : UPGRADE_STORAGE_CORRUPT;
}

/* 取消只释放已匹配租约；提交可能发生时，先证明仍为原完整基线。 */
static UpgradeStorageResult upg_abort(const UpgradeStorageRequest *req)
{
    if (req->tx_id == 0U) return UPGRADE_STORAGE_INVALID;
    if (!s_tx.active) return UPGRADE_STORAGE_OK;
    if (req->tx_id != s_tx.tx_id || (req->session != 0U && req->session != s_tx.session))
        return UPGRADE_STORAGE_CONFLICT;
    if (s_tx.commit_attempted) {
        // 提交尝试后取消也须先证明仍是原完整基线；超时或应答失败本身不能解除候选保护。
        UpgradeStateProtection view;
        UpgradeStorageResult st = upg_state_result(
            UpgradeState_ReadProtection(upg_read_metadata, NULL, &view));
        if (st != UPGRADE_STORAGE_OK) return st;
        if (view.recovery_result != UPGRADE_STATE_EMPTY ||
            !upg_same_record(&view.normal, &s_tx.baseline)) return UPGRADE_STORAGE_CONFLICT;
    }
    memset(&s_tx, 0, sizeof(s_tx));
    return UPGRADE_STORAGE_OK;
}

/* 登记真实存储任务，不扫描、格式化或初始化任何持久记录。 */
void storage_upgrade_init(bool flash_ready)
{
    s_owner = osThreadGetId();
    s_ready = flash_ready;
}

/*
 * 由升级队列处理函数在 Storage_Task 内调用，先检查所有者、期限和类型，再分发实际介质操作。
 * BEGIN 准备候选槽，CHUNK 写入并读回，COMMIT_READY 写安装意图；未知或到期请求不做 I/O。
 * rsp 带回原请求身份、执行结果及累计 written，客户端须匹配完整应答后才能推进。
 */
void storage_upgrade_handle(const UpgradeStorageRequest *req, UpgradeStorageResponse *rsp)
{
    memset(rsp, 0, sizeof(*rsp));
    rsp->req_id = req->req_id;
    rsp->session = req->session;
    rsp->tx_id = req->tx_id;
    rsp->type = req->type;
    s_deadline = req->deadline_ms;
    s_io_result = UPGRADE_STORAGE_OK;
    s_detail = 0U;
    UpgradeStorageResult st = UPGRADE_STORAGE_INVALID;

    bool known = req->type <= UPGRADE_STORAGE_COMMIT_READY && req->type != 2U;
    if (!known || req->req_id == 0U) {
        st = UPGRADE_STORAGE_INVALID;
    } else if (!s_ready || osThreadGetId() != s_owner) {
        st = UPGRADE_STORAGE_NOT_READY;
    } else if (upg_expired()) {
        st = UPGRADE_STORAGE_TIMEOUT;
    } else {
        switch (req->type) {
        case UPGRADE_STORAGE_BEGIN: st = upg_begin(req, rsp); break;
        case UPGRADE_STORAGE_CHUNK: st = upg_chunk(req); break;
        case UPGRADE_STORAGE_ABORT: st = upg_abort(req); break;
        case UPGRADE_STORAGE_READ_PACKAGE: st = upg_read_package(req, rsp); break;
        case UPGRADE_STORAGE_READ_METADATA:
            if (req->session != 0U || req->tx_id != 0U) break;
            if (req->len == 0U || req->len > UPGRADE_STORAGE_CHUNK_SIZE ||
                req->offset >= UPGRADE_STATE_METADATA_SIZE ||
                req->len > UPGRADE_STATE_METADATA_SIZE - req->offset) break;
            st = upg_read_metadata(NULL, req->offset, rsp->data, req->len)
               ? UPGRADE_STORAGE_OK : s_io_result;
            if (st == UPGRADE_STORAGE_OK) rsp->actual_len = req->len;
            break;
        case UPGRADE_STORAGE_RESERVE_TX:
            if (req->session == 0U && req->tx_id == 0U) st = upg_reserve(rsp);
            break;
        case UPGRADE_STORAGE_COMMIT_READY: st = upg_commit(req); break;
        default: break;
        }
    }
    rsp->result = st;
    rsp->detail = s_detail;
    rsp->written = s_tx.active ? s_tx.written : 0U;
    rsp->session_ended = s_tx.active ? 0U : 1U;
}
