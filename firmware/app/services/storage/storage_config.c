/*
 * storage_config.c — A/B/C配置事务；Storage_Task独占介质。
 * 本次启动固定运行槽R；保存只更新S，候选不得覆盖R或最近一次S。
 */
#include "storage_runtime.h"
#include "storage_config.h"
#include "config_slot.h"
#include "flash_partition.h"
#include <string.h>

static const uint32_t slot_base[3] = {
    FLASH_CONFIG_SLOT_A_BASE, FLASH_CONFIG_SLOT_B_BASE, FLASH_CONFIG_SLOT_C_BASE
};
#define JSON_DATA_OFFSET sizeof(config_slot_header_t)
#define COMMIT_MARKER_OFFSET (FLASH_CONFIG_SLOT_SIZE - 4U)

typedef struct {
    bool active;
    bool failed;
    bool validated;
    bool commit_attempted;
    uint32_t session_id;
    int slot_idx;
    uint32_t total_len;
    uint32_t written_len;
    uint32_t crc;
} config_tx_t;

static config_tx_t s_tx;
static int s_r_slot = -1;
static int s_s_slot = -1;
static uint32_t s_r_gen;
static uint32_t s_s_gen;
static uint32_t s_next_session;
static uint32_t s_committed_session;
static bool s_scan_error;
static bool s_scanned;

/* 增量CRC-32/ISO-HDLC，调用方负责初值和终值异或。 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8U; bit++)
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
    return crc;
}

/* 只读计算指定槽JSON的CRC，不把整份配置放入RAM。 */
static HAL_StatusTypeDef slot_crc(int slot, uint32_t len, uint32_t *value)
{
    uint8_t data[256];
    uint32_t crc = UINT32_MAX;
    for (uint32_t offset = 0; offset < len;) {
        uint32_t count = len - offset;
        if (count > sizeof(data)) count = sizeof(data);
        HAL_StatusTypeDef st = storage_flash_read(slot_base[slot] + JSON_DATA_OFFSET + offset, data, count);
        if (st != HAL_OK) return st;
        crc = crc32_update(crc, data, count);
        offset += count;
    }
    *value = ~crc;
    return HAL_OK;
}

/* 只检查头部字段，不把坏长度或代次当作无配置。 */
static bool slot_header_valid(const config_slot_header_t *header)
{
    return header->magic == CONFIG_SLOT_MAGIC && header->generation != 0U &&
           header->json_length != 0U && header->json_length <= FLASH_CONFIG_JSON_MAX &&
           header->header_crc32 == ~crc32_update(UINT32_MAX, (const uint8_t *)header, 16U);
}

/* 完整槽同时要求提交标记、头CRC和实际JSON CRC有效。 */
static HAL_StatusTypeDef slot_check(int slot, config_slot_header_t *header, bool *complete)
{
    uint32_t marker;
    *complete = false;
    HAL_StatusTypeDef st = storage_flash_read(slot_base[slot] + COMMIT_MARKER_OFFSET,
                                              (uint8_t *)&marker, sizeof(marker));
    if (st != HAL_OK) return st;
    if (marker != CONFIG_SLOT_MARKER_VALID) return HAL_OK;
    st = storage_flash_read(slot_base[slot], (uint8_t *)header, sizeof(*header));
    if (st != HAL_OK) return st;
    if (!slot_header_valid(header)) return HAL_ERROR;
    uint32_t crc;
    st = slot_crc(slot, header->json_length, &crc);
    if (st != HAL_OK) return st;
    if (crc != header->json_crc32) return HAL_ERROR;
    *complete = true;
    return HAL_OK;
}

/* 仅启动选槽；运行中保存不能重新选择R。读取失败不冒充空配置。 */
static void config_select_running(void)
{
    s_scan_error = false;
    int best = -1;
    uint32_t generation = 0;
    for (int slot = 0; slot < 3; slot++) {
        config_slot_header_t header;
        bool complete;
        if (slot_check(slot, &header, &complete) != HAL_OK) {
            s_scan_error = true;
            continue;
        }
        if (complete && (best < 0 || header.generation > generation)) {
            best = slot;
            generation = header.generation;
        } else if (complete && header.generation == generation) {
            s_scan_error = true;
        }
    }
    s_r_slot = s_scan_error ? -1 : best;
    s_r_gen = s_scan_error ? 0U : generation;
    s_s_slot = s_r_slot;
    s_s_gen = s_r_gen;
    s_scanned = true;
}

/* 打开本次启动固定的运行槽；无配置和I/O失败均不输出旧句柄。 */
HAL_StatusTypeDef storage_config_open(storage_config_reader_t *reader)
{
    if (reader == NULL || !storage_runtime_is_owner()) return HAL_ERROR;
    memset(reader, 0, sizeof(*reader));
    if (!s_scanned) config_select_running();
    if (s_scan_error || s_r_slot < 0) return HAL_ERROR;
    config_slot_header_t header;
    HAL_StatusTypeDef st = storage_flash_read(slot_base[s_r_slot], (uint8_t *)&header, sizeof(header));
    if (st != HAL_OK) return st;
    if (!slot_header_valid(&header) || header.generation != s_r_gen) return HAL_ERROR;
    reader->slot_idx = (uint32_t)s_r_slot;
    reader->generation = s_r_gen;
    reader->total_len = header.json_length;
    reader->crc32 = header.json_crc32;
    return HAL_OK;
}

/* 读句柄只允许指向受保护的R/S，不接受任意Flash地址或过期候选。 */
HAL_StatusTypeDef storage_config_read_chunk(const storage_config_reader_t *reader,
                                            uint32_t offset, uint8_t *data, uint32_t len)
{
    if (!storage_runtime_is_owner() || reader == NULL || data == NULL ||
        reader->slot_idx > 2U || reader->total_len > FLASH_CONFIG_JSON_MAX ||
        offset > reader->total_len || len > reader->total_len - offset)
        return HAL_ERROR;
    int slot = (int)reader->slot_idx;
    if (!((slot == s_r_slot && reader->generation == s_r_gen) ||
          (slot == s_s_slot && reader->generation == s_s_gen))) return HAL_ERROR;
    return storage_flash_read(slot_base[slot] + JSON_DATA_OFFSET + offset, data, len);
}

/* 擦除前登记候选租约，擦写失败也保留归属供原客户端取消。 */
static HAL_StatusTypeDef config_begin(uint32_t total_len, uint32_t session)
{
    if (!s_scanned || s_scan_error || total_len == 0U || total_len > FLASH_CONFIG_JSON_MAX ||
        session == 0U || session <= s_next_session || s_s_gen == UINT32_MAX) return HAL_ERROR;
    if (s_tx.active) return HAL_BUSY;
    int candidate = -1;
    for (int slot = 0; slot < 3; slot++)
        if (slot != s_r_slot && slot != s_s_slot) { candidate = slot; break; }
    if (candidate < 0) return HAL_ERROR;
    s_next_session = session;
    s_tx = (config_tx_t){.active = true, .session_id = session, .slot_idx = candidate,
                         .total_len = total_len, .crc = UINT32_MAX};
    /* 先擦尾标所在扇区；断电不能留下“有效提交标记+已擦坏正文”。 */
    HAL_StatusTypeDef st = storage_flash_erase(slot_base[candidate] + FLASH_CONFIG_SLOT_SIZE - 4096U);
    uint32_t marker = 0U;
    if (st == HAL_OK)
        st = storage_flash_read(slot_base[candidate] + COMMIT_MARKER_OFFSET,
                                 (uint8_t *)&marker, sizeof(marker));
    if (st != HAL_OK || marker != CONFIG_SLOT_MARKER_EMPTY) {
        s_tx.failed = true;
        return st == HAL_OK ? HAL_ERROR : st;
    }
    for (uint32_t offset = 0; offset < FLASH_CONFIG_SLOT_SIZE - 4096U; offset += 4096U) {
        st = storage_flash_erase(slot_base[candidate] + offset);
        if (st != HAL_OK) { s_tx.failed = true; return st; }
    }
    return HAL_OK;
}

/* 分配非零会话；失败可能仍有租约，调用方须用输出会话取消。 */
HAL_StatusTypeDef storage_config_begin(uint32_t total_len, uint32_t *session_id)
{
    if (session_id == NULL) return HAL_ERROR;
    *session_id = 0U;
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = HAL_ERROR;
    if (s_next_session != UINT32_MAX) {
        *session_id = s_next_session + 1U;
        st = config_begin(total_len, *session_id);
    }
    storage_mutation_end();
    return st;
}

/* 顺序写入并读回；不明写入锁存失败，不推进偏移或接受重写。 */
HAL_StatusTypeDef storage_config_chunk(uint32_t session, uint32_t offset,
                                        const uint8_t *data, uint32_t len)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = HAL_ERROR;
    if (s_tx.active && !s_tx.failed && !s_tx.validated && session == s_tx.session_id &&
        data != NULL && len != 0U && len <= STORAGE_CONFIG_CHUNK_MAX &&
        offset == s_tx.written_len && offset <= s_tx.total_len && len <= s_tx.total_len - offset) {
        uint8_t readback[STORAGE_CONFIG_CHUNK_MAX];
        uint32_t address = slot_base[s_tx.slot_idx] + JSON_DATA_OFFSET + offset;
        st = storage_flash_write(address, data, len);
        if (st == HAL_OK) st = storage_flash_read(address, readback, len);
        if (st == HAL_OK && memcmp(data, readback, len) != 0) st = HAL_ERROR;
        if (st == HAL_OK) {
            s_tx.crc = crc32_update(s_tx.crc, data, len);
            s_tx.written_len += len;
        } else s_tx.failed = true;
    }
    storage_mutation_end();
    return st;
}

/* 验证读回CRC与接纳的输入一致，空操作不构成校验成功。 */
HAL_StatusTypeDef storage_config_validate(uint32_t session)
{
    if (!storage_runtime_is_owner()) return HAL_ERROR;
    if (!s_tx.active || s_tx.failed || session != s_tx.session_id ||
        s_tx.written_len != s_tx.total_len) return HAL_ERROR;
    uint32_t crc;
    HAL_StatusTypeDef st = slot_crc(s_tx.slot_idx, s_tx.total_len, &crc);
    if (st == HAL_OK && crc != ~s_tx.crc) st = HAL_ERROR;
    s_tx.validated = st == HAL_OK;
    if (st != HAL_OK) s_tx.failed = true;
    return st;
}

/* 只读解析曾尝试提交的候选；未知结果保留租约，不能重新擦写。 */
static HAL_StatusTypeDef config_resolve(uint32_t session, bool *committed)
{
    *committed = session != 0U && session == s_committed_session;
    if (*committed) return HAL_OK;
    if (!s_tx.active || session != s_tx.session_id) return HAL_ERROR;
    if (!s_tx.commit_attempted) return HAL_OK;
    config_slot_header_t header;
    bool complete;
    HAL_StatusTypeDef st = slot_check(s_tx.slot_idx, &header, &complete);
    if (st != HAL_OK) return st;
    if (!complete) {
        s_tx.commit_attempted = false;
        s_tx.failed = true;
        return HAL_OK;
    }
    if (header.generation != s_s_gen + 1U || header.json_length != s_tx.total_len ||
        header.json_crc32 != ~s_tx.crc) return HAL_ERROR;
    s_s_slot = s_tx.slot_idx;
    s_s_gen = header.generation;
    s_committed_session = session;
    s_tx.active = false;
    *committed = true;
    return HAL_OK;
}

/* 先提交头并读回，最后写标记；成功只切换S，R保持到下一次启动。 */
HAL_StatusTypeDef storage_config_commit(uint32_t session)
{
    if (!storage_runtime_is_owner()) return HAL_ERROR;
    if (session != 0U && session == s_committed_session) return HAL_OK;
    if (s_tx.active && session == s_tx.session_id && s_tx.commit_attempted) {
        bool committed;
        HAL_StatusTypeDef st = config_resolve(session, &committed);
        return st == HAL_OK && !committed ? HAL_ERROR : st;
    }
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = HAL_ERROR;
    if (s_tx.active && !s_tx.failed && s_tx.validated && session == s_tx.session_id) {
        config_slot_header_t header = {0};
        header.magic = CONFIG_SLOT_MAGIC;
        header.generation = s_s_gen + 1U;
        header.json_length = s_tx.total_len;
        header.json_crc32 = ~s_tx.crc;
        header.header_crc32 = ~crc32_update(UINT32_MAX, (const uint8_t *)&header, 16U);
        config_slot_header_t readback;
        st = storage_flash_write(slot_base[s_tx.slot_idx], (const uint8_t *)&header, sizeof(header));
        if (st == HAL_OK) st = storage_flash_read(slot_base[s_tx.slot_idx], (uint8_t *)&readback, sizeof(readback));
        if (st == HAL_OK && memcmp(&header, &readback, sizeof(header)) != 0) st = HAL_ERROR;
        if (st == HAL_OK) {
            uint32_t marker = CONFIG_SLOT_MARKER_VALID;
            s_tx.commit_attempted = true;
            st = storage_flash_write(slot_base[s_tx.slot_idx] + COMMIT_MARKER_OFFSET,
                                      (const uint8_t *)&marker, sizeof(marker));
            bool committed;
            HAL_StatusTypeDef resolved = config_resolve(session, &committed);
            if (resolved != HAL_OK) st = resolved;
            else st = committed ? HAL_OK : HAL_ERROR;
        } else s_tx.failed = true;
    }
    storage_mutation_end();
    return st;
}

/* 取消只释放匹配的未提交租约，允许在升级暂停后结算，不擦介质。 */
static HAL_StatusTypeDef config_abort(uint32_t session)
{
    if (!storage_runtime_is_owner() || session == 0U) return HAL_ERROR;
    if (session == s_committed_session) return HAL_BUSY;
    if (session != s_tx.session_id) return s_tx.active ? HAL_BUSY : HAL_OK;
    if (s_tx.commit_attempted) return HAL_BUSY;
    s_tx.active = false;
    return HAL_OK;
}

/* 兼容已有存储侧取消入口；客户端使用process取得明确结果。 */
void storage_config_abort(uint32_t session)
{
    (void)config_abort(session);
}

/* 运行generation不随保存改变。 */
uint32_t storage_config_get_generation(void)
{
    return s_r_gen;
}

/* 全空为未配置，不能在首次保存后通过重新扫描激活候选。 */
bool storage_config_has_valid(void)
{
    if (!storage_runtime_is_owner()) return false;
    if (!s_scanned) config_select_running();
    return !s_scan_error && s_r_slot >= 0;
}

/* 启动执行一次完整只读恢复；不迁移、不擦除未知配置。 */
HAL_StatusTypeDef storage_config_check(void)
{
    if (!storage_runtime_is_owner()) return HAL_ERROR;
    if (!s_scanned) config_select_running();
    return s_scan_error ? HAL_ERROR : HAL_OK;
}

/* 未结束的候选或不明提交均阻止Storage升级结算。 */
bool storage_config_tx_active(void)
{
    return s_tx.active;
}

/* Storage_Task处理Config值拷贝请求；不保留消息或客户端缓冲。 */
HAL_StatusTypeDef storage_config_process(storage_config_message_t *message)
{
    if (!storage_runtime_is_owner() || message == NULL) return HAL_ERROR;
    if (message->action == STORAGE_CONFIG_READ_RUNNING || message->action == STORAGE_CONFIG_READ_SAVED) {
        if (!s_scanned || s_scan_error || message->len > sizeof(message->data)) return HAL_ERROR;
        bool saved = message->action == STORAGE_CONFIG_READ_SAVED;
        int slot = saved ? s_s_slot : s_r_slot;
        message->generation = saved ? s_s_gen : s_r_gen;
        message->total_len = 0U;
        message->crc32 = 0U;
        if (slot < 0) { message->len = 0U; return HAL_OK; }
        config_slot_header_t header;
        HAL_StatusTypeDef st = storage_flash_read(slot_base[slot], (uint8_t *)&header, sizeof(header));
        if (st != HAL_OK) return st;
        if (!slot_header_valid(&header) || header.generation != message->generation) return HAL_ERROR;
        message->total_len = header.json_length;
        message->crc32 = header.json_crc32;
        if (message->offset > header.json_length) return HAL_ERROR;
        if (message->len > header.json_length - message->offset)
            message->len = header.json_length - message->offset;
        return message->len == 0U ? HAL_OK :
               storage_flash_read(slot_base[slot] + JSON_DATA_OFFSET + message->offset,
                                   message->data, message->len);
    }
    if (message->action == STORAGE_CONFIG_RESOLVE) {
        bool committed;
        HAL_StatusTypeDef st = config_resolve(message->session_id, &committed);
        message->generation = committed ? s_s_gen : 0U;
        return st;
    }
    if (message->action == STORAGE_CONFIG_ABORT) return config_abort(message->session_id);
    if (message->action == STORAGE_CONFIG_BEGIN) {
        if (!storage_mutation_begin()) return HAL_BUSY;
        HAL_StatusTypeDef st = config_begin(message->total_len, message->session_id);
        message->generation = s_s_gen + 1U;
        storage_mutation_end();
        return st;
    }
    if (message->action == STORAGE_CONFIG_CHUNK)
        return storage_config_chunk(message->session_id, message->offset, message->data, message->len);
    if (message->action == STORAGE_CONFIG_VALIDATE) return storage_config_validate(message->session_id);
    if (message->action == STORAGE_CONFIG_COMMIT) return storage_config_commit(message->session_id);
    return HAL_ERROR;
}
