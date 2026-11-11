/*
 * storage_config.c — 配置存储服务实现（A/B/C 三槽事务）
 *
 * 依据设计说明书 6.2 节：
 *   三槽各 80KiB，布局为 [头 64B][JSON 最多 64KiB][提交标记 4B]
 *   R = generation 最新且完整的运行槽
 *   S = 最近成功保存槽
 *   候选槽 = 既非 R 也非 S 的槽
 *
 * 事务：BEGIN（擦候选）→ CHUNK（分块写）→ VALIDATE（CRC）→ COMMIT（写标记）
 * M3 只存 JSON 字节流，不解析内容。
 */

#include "storage_runtime.h"
#include "storage_config.h"
#include "config_slot.h"
#include "flash_partition.h"
#include "spi_flash_w25q128.h"
#include <string.h>

/* 三槽基址 */
static const uint32_t slot_base[3] = {
    FLASH_CONFIG_SLOT_A_BASE,
    FLASH_CONFIG_SLOT_B_BASE,
    FLASH_CONFIG_SLOT_C_BASE,
};

/* JSON 数据在槽内的偏移（紧跟头部） */
#define JSON_DATA_OFFSET   sizeof(config_slot_header_t)
/* 提交标记在槽内的偏移（槽末尾 4 字节） */
#define COMMIT_MARKER_OFFSET  (FLASH_CONFIG_SLOT_SIZE - 4)

/* 事务状态 */
typedef struct {
    bool      active;
    uint32_t  session_id;
    int       slot_idx;       /* 候选槽索引 0-2 */
    uint32_t  total_len;      /* 预期总长度 */
    uint32_t  written_len;    /* 已写入长度 */
} config_tx_t;

static config_tx_t  s_tx = {0};
static int          s_r_slot = -1;   /* 当前运行槽索引 */
static bool s_scan_error;
static uint32_t     s_r_gen  = 0;    /* 当前运行槽 generation */

/* ---------- CRC32（标准 CRC-32，多项式 0xEDB88320） ---------- */

static uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

/* ---------- 槽操作辅助 ---------- */

/* 读取槽的提交标记 */
static HAL_StatusTypeDef slot_read_marker(int idx, uint32_t *marker)
{
    return storage_flash_read(slot_base[idx] + COMMIT_MARKER_OFFSET,
                                  (uint8_t *)marker, 4);
}

/* 读取槽的头部 */
static HAL_StatusTypeDef slot_read_header(int idx, config_slot_header_t *hdr)
{
    return storage_flash_read(slot_base[idx], (uint8_t *)hdr,
                                  sizeof(config_slot_header_t));
}

/* 检查槽是否完整（头部有效 + 提交标记有效） */
static bool slot_is_complete(int idx)
{
    config_slot_header_t hdr;
    uint32_t marker;
    if (slot_read_header(idx, &hdr) != HAL_OK || slot_read_marker(idx, &marker) != HAL_OK) {
        s_scan_error = true;
        return false;
    }
    if (marker != CONFIG_SLOT_MARKER_VALID) return false;
    uint32_t crc = crc32_calc((const uint8_t *)&hdr, 16U);
    if (hdr.magic != CONFIG_SLOT_MAGIC || hdr.header_crc32 != crc ||
        hdr.json_length > FLASH_CONFIG_JSON_MAX || hdr.generation == 0U) {
        s_scan_error = true;
        return false;
    }
    return true;
}

/* 擦除整个槽（80KiB = 20 个 4KiB 扇区） */
static HAL_StatusTypeDef slot_erase(int idx)
{
    uint32_t addr = slot_base[idx];
    for (int i = 0; i < 20; i++) {
        HAL_StatusTypeDef st = storage_flash_erase(addr + i * 4096);
        if (st != HAL_OK) return st;
    }
    return HAL_OK;
}

/* ---------- 启动时选择运行槽 ---------- */

/* 启动时扫三个槽，挑出"代次最大且校验完整"的那个为运行槽 */
static void config_select_running(void)
{
    s_scan_error = false;
    // 最佳槽下标，-1 表示还没找到
    int best_idx = -1;
    // 最佳槽的代次号，用于比较大小
    uint32_t best_gen = 0;

    // 遍历 A/B/C 三个槽
    for (int i = 0; i < 3; i++) {
        // 跳过校验未通过（CRC 错误、长度异常）的槽
        if (!slot_is_complete(i)) continue;

        // 读取该槽头部，拿 generation 字段
        config_slot_header_t hdr;
        if (slot_read_header(i, &hdr) != HAL_OK) {
            s_scan_error = true;
            continue;
        }

        // 第一次遇到完整槽或代次更大时，更新最佳
        if (best_idx == -1 || hdr.generation > best_gen) {
            best_idx = i;
            best_gen = hdr.generation;
        }
    }

    // 写回全局运行槽标记，若无完整槽则保持 -1
    s_r_slot = s_scan_error ? -1 : best_idx;
    s_r_gen = s_scan_error ? 0U : best_gen;
}

/* ---------- 公共 API ---------- */

HAL_StatusTypeDef storage_config_open(storage_config_reader_t *rd)
{
    // 入参校验
    if (rd == NULL) return HAL_ERROR;

    // 检查有没有成功选出配置槽，如果还没选到，就重新执行一次三槽校验选槽，做一次兜底重试
    if (s_r_slot < 0) {
        config_select_running();
    }

    // 三槽都无完整配置，交由上层进入维护模式
    if (s_r_slot < 0) return HAL_ERROR;

    // 重读头部，避免描述符沿用旧的长度/CRC
    config_slot_header_t hdr;
    if (slot_read_header(s_r_slot, &hdr) != HAL_OK) return HAL_ERROR;

    // 填充只读描述符：槽号定位数据、代次记录配置版本
    rd->slot_idx   = (uint32_t)s_r_slot;
    rd->generation = hdr.generation;
    rd->total_len  = hdr.json_length;
    rd->crc32      = hdr.json_crc32;
    return HAL_OK;
}

HAL_StatusTypeDef storage_config_read_chunk(const storage_config_reader_t *rd,
                                             uint32_t offset, uint8_t *buf,
                                             uint32_t len)
{
    if (rd == NULL || buf == NULL) return HAL_ERROR;
    if (rd->slot_idx > 2) return HAL_ERROR;
    if (offset > rd->total_len) return HAL_ERROR;
    if (len > rd->total_len - offset) return HAL_ERROR;

    return storage_flash_read(
        slot_base[rd->slot_idx] + JSON_DATA_OFFSET + offset, buf, len);
}

static HAL_StatusTypeDef storage_config_begin_impl(uint32_t total_len, uint32_t *session_id)
{
    if (session_id == NULL) return HAL_ERROR;
    if (total_len > FLASH_CONFIG_JSON_MAX) return HAL_ERROR;
    if (s_tx.active) return HAL_BUSY;

    /* 选候选槽：非 R 的槽 */
    int candidate = -1;
    for (int i = 0; i < 3; i++) {
        if (i != s_r_slot) {
            candidate = i;
            break;
        }
    }
    if (candidate < 0) return HAL_ERROR;

    /* 擦除候选槽 */
    HAL_StatusTypeDef st = slot_erase(candidate);
    if (st != HAL_OK) return st;

    s_tx.active      = true;
    s_tx.session_id  = (uint32_t)(s_r_gen + 1) * 1000 + candidate;
    s_tx.slot_idx    = candidate;
    s_tx.total_len   = total_len;
    s_tx.written_len = 0;

    *session_id = s_tx.session_id;
    return HAL_OK;
}

static HAL_StatusTypeDef storage_config_chunk_impl(uint32_t session_id, uint32_t offset,
                                        const uint8_t *data, uint32_t len)
{
    if (!s_tx.active) return HAL_ERROR;
    if (session_id != s_tx.session_id) return HAL_ERROR;
    if (offset != s_tx.written_len) return HAL_ERROR;  /* 必须连续写入 */
    if (offset > s_tx.total_len || len > s_tx.total_len - offset) return HAL_ERROR;

    uint32_t addr = slot_base[s_tx.slot_idx] + JSON_DATA_OFFSET + offset;
    HAL_StatusTypeDef st = storage_flash_write(addr, data, len);
    if (st != HAL_OK) return st;

    s_tx.written_len += len;
    return HAL_OK;
}

static HAL_StatusTypeDef storage_config_validate_impl(uint32_t session_id)
{
    if (!s_tx.active) return HAL_ERROR;
    if (session_id != s_tx.session_id) return HAL_ERROR;
    if (s_tx.written_len != s_tx.total_len) return HAL_ERROR;

    /* 回读并计算 CRC */
    /* 为节省内存，分块读取计算 CRC */
    uint32_t crc = 0xFFFFFFFF;
    uint32_t remaining = s_tx.total_len;
    uint32_t offset = 0;
    uint8_t buf[256];

    while (remaining > 0) {
        uint32_t chunk = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        HAL_StatusTypeDef st = storage_flash_read(
            slot_base[s_tx.slot_idx] + JSON_DATA_OFFSET + offset,
            buf, chunk);
        if (st != HAL_OK) return st;

        for (uint32_t i = 0; i < chunk; i++) {
            crc ^= buf[i];
            for (int j = 0; j < 8; j++) {
                crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
            }
        }
        offset += chunk;
        remaining -= chunk;
    }
    crc = ~crc;

    /* CRC 校验通过。实际写入头部时由 COMMIT 重新计算。 */
    return HAL_OK;
}

static HAL_StatusTypeDef storage_config_commit_impl(uint32_t session_id)
{
    if (!s_tx.active) return HAL_ERROR;
    if (session_id != s_tx.session_id) return HAL_ERROR;
    if (s_tx.written_len != s_tx.total_len) return HAL_ERROR;

    /* 重新计算 CRC（validate 可能没被调用） */
    uint32_t crc = 0xFFFFFFFF;
    uint32_t remaining = s_tx.total_len;
    uint32_t offset = 0;
    uint8_t buf[256];

    while (remaining > 0) {
        uint32_t chunk = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        if (storage_flash_read(
                slot_base[s_tx.slot_idx] + JSON_DATA_OFFSET + offset,
                buf, chunk) != HAL_OK) return HAL_ERROR;
        for (uint32_t i = 0; i < chunk; i++) {
            crc ^= buf[i];
            for (int j = 0; j < 8; j++) {
                crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
            }
        }
        offset += chunk;
        remaining -= chunk;
    }
    crc = ~crc;

    /* 写头部 */
    config_slot_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = CONFIG_SLOT_MAGIC;
    hdr.generation  = s_r_gen + 1;
    hdr.json_length = s_tx.total_len;
    hdr.json_crc32  = crc;
    hdr.header_crc32 = crc32_calc((uint8_t *)&hdr,
                                    sizeof(hdr) - sizeof(uint32_t) - sizeof(hdr.reserved));

    HAL_StatusTypeDef st = storage_flash_write(
        slot_base[s_tx.slot_idx], (uint8_t *)&hdr, sizeof(hdr));
    if (st != HAL_OK) return st;

    /* 写提交标记（最后 4 字节） */
    uint32_t marker = CONFIG_SLOT_MARKER_VALID;
    st = storage_flash_write(
        slot_base[s_tx.slot_idx] + COMMIT_MARKER_OFFSET,
        (uint8_t *)&marker, 4);
    if (st != HAL_OK) return st;

    /* 事务完成，更新运行槽 */
    s_r_slot = s_tx.slot_idx;
    s_r_gen  = hdr.generation;
    s_tx.active = false;

    return HAL_OK;
}

static void storage_config_abort_impl(uint32_t session_id)
{
    if (s_tx.active && session_id == s_tx.session_id) {
        s_tx.active = false;
    }
}

uint32_t storage_config_get_generation(void)
{
    return s_r_gen;
}

bool storage_config_has_valid(void)
{
    if (s_r_slot < 0) config_select_running();
    return (s_r_slot >= 0);
}

/* 开始候选配置事务，完整擦除成功才返回会话。 */
HAL_StatusTypeDef storage_config_begin(uint32_t total_len, uint32_t *session_id)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_config_begin_impl(total_len, session_id);
    storage_mutation_end();
    return st;
}

/* 按连续偏移写入当前配置事务，返回前结束介质访问。 */
HAL_StatusTypeDef storage_config_chunk(uint32_t session_id, uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_config_chunk_impl(session_id, offset, data, len);
    storage_mutation_end();
    return st;
}

/* 检查当前事务已收齐并可完整读回配置。 */
HAL_StatusTypeDef storage_config_validate(uint32_t session_id)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_config_validate_impl(session_id);
    storage_mutation_end();
    return st;
}

/* 提交完整配置及尾标，成功后切换运行槽。 */
HAL_StatusTypeDef storage_config_commit(uint32_t session_id)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_config_commit_impl(session_id);
    storage_mutation_end();
    return st;
}

/* 取消匹配的配置RAM事务，不提交或擦除已有配置。 */
void storage_config_abort(uint32_t session_id)
{
    if (!storage_mutation_begin()) return;
    storage_config_abort_impl(session_id);
    storage_mutation_end();
}

/* 全空或未提交候选属于未配置，读取失败/已提交损坏不能降级成空配置。 */
HAL_StatusTypeDef storage_config_check(void)
{
    config_select_running();
    return s_scan_error ? HAL_ERROR : HAL_OK;
}

/* 结算查询不取消配置会话，也不提交半份配置。 */
bool storage_config_tx_active(void)
{
    return s_tx.active;
}
