/*
 * storage_ctrllog.c — 控制日志（双区定长槽 + 整区回收）
 *
 * 详见 storage_ctrllog.h 的槽格式说明。
 *
 * 关键约束：
 *   - 写槽顺序：先写 header+payload+crc，最后单独写提交尾标 commit。
 *     掉电中断时 commit 不完整，读取方因 commit/crc 校验失败自动丢弃半成品。
 *   - 当前区写满后只回收无已提交记录的另一区，不提前遗忘有效记录。
 *   - 非零去重键查询两个保留区；同键异内容报错，不以RAM缓存代替持久记录。
 */

#include "storage_runtime.h"
#include "storage_ctrllog.h"
#include "flash_partition.h"
#include "spi_flash_w25q128.h"
#include "storage_seq.h"
#include "storage_time.h"

#include <string.h>

#define CTRLLOG_MAGIC      0x474C5443U  /* "CTLG" */
#define CTRLLOG_COMMIT     0x54494D43U  /* "CMIT" */

#define CTRLLOG_HEADER_SIZE 32U
#define CTRLLOG_COMMIT_OFF  (CTRLLOG_SLOT_SIZE - sizeof(uint32_t))
#define CTRLLOG_SLOT_COUNT  (FLASH_CTRLLOG_AREA_SIZE / CTRLLOG_SLOT_SIZE)

/* header 字段偏移 */
#define OFF_MAGIC   0U
#define OFF_VERSION 4U
#define OFF_STREAM  6U
#define OFF_LEN     8U
#define OFF_FLAGS   10U
#define OFF_TS      12U
#define OFF_SEQ     16U
#define OFF_KEY     24U
#define OFF_PAYLOAD 32U

/* ---------- 内部状态 ---------- */

static bool     s_inited   = false;
static int      s_active   = 0;   /* 当前写入区 */
static uint32_t s_next_slot = 0;  /* 当前区下一个候选槽 */
static uint64_t s_last_seq = 0;
static uint32_t s_erase_offset;
static int      s_erasing  = -1;  /* >=0 表示正在回收该区 */

/* ---------- 工具函数 ---------- */

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

static uint16_t get_u16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void put_u16(uint8_t *p, uint16_t v) { memcpy(p, &v, sizeof(v)); }
static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
static void put_u64(uint8_t *p, uint64_t v) { memcpy(p, &v, sizeof(v)); }

static uint32_t area_base(int zone)
{
    return (zone == 0) ? FLASH_CTRLLOG_AREA0_BASE : FLASH_CTRLLOG_AREA1_BASE;
}

static uint32_t slot_addr(int zone, uint32_t index)
{
    return area_base(zone) + index * CTRLLOG_SLOT_SIZE;
}

static bool slot_is_erased(const uint8_t *buf)
{
    for (uint32_t i = 0; i < CTRLLOG_SLOT_SIZE; i++) {
        if (buf[i] != 0xFF) return false;
    }
    return true;
}

/* 校验并解析一个槽；成功则填充 rec（payload 指向 buf 内部） */
static bool parse_slot(const uint8_t *buf, ctrllog_record_t *rec)
{
    if (get_u32(buf + OFF_MAGIC) != CTRLLOG_MAGIC) return false;
    if (get_u32(buf + CTRLLOG_COMMIT_OFF) != CTRLLOG_COMMIT) return false;

    uint16_t version = get_u16(buf + OFF_VERSION);
    uint16_t len     = get_u16(buf + OFF_LEN);
    if (version != CTRLLOG_FORMAT_VERSION) return false;
    if (len > CTRLLOG_PAYLOAD_MAX) return false;

    uint32_t crc_off = CTRLLOG_HEADER_SIZE + len;
    uint32_t crc = get_u32(buf + crc_off);
    if (crc != crc32_calc(buf, crc_off)) return false;

    if (rec != NULL) {
        rec->format_version = version;
        rec->stream         = get_u16(buf + OFF_STREAM);
        rec->payload_len    = len;
        rec->flags          = get_u16(buf + OFF_FLAGS);
        rec->timestamp      = get_u32(buf + OFF_TS);
        rec->seq            = get_u64(buf + OFF_SEQ);
        rec->dedup_key      = get_u64(buf + OFF_KEY);
        rec->payload        = buf + OFF_PAYLOAD;
    }
    return true;
}

/* 扫描一个区：返回有效记录数，最大 seq 及所在槽位；SPI 读失败返回错误 */
static HAL_StatusTypeDef scan_zone(int zone, uint32_t *count,
                                   uint64_t *max_seq, uint32_t *max_seq_slot)
{
    uint8_t buf[CTRLLOG_SLOT_SIZE];

    *count = 0;
    *max_seq = 0;
    *max_seq_slot = 0;

    for (uint32_t i = 0; i < CTRLLOG_SLOT_COUNT; i++) {
        if (storage_flash_read(slot_addr(zone, i), buf, CTRLLOG_SLOT_SIZE) != HAL_OK)
            return HAL_ERROR;

        ctrllog_record_t rec;
        if (!parse_slot(buf, &rec)) {
            if (get_u32(buf + CTRLLOG_COMMIT_OFF) == CTRLLOG_COMMIT) return HAL_ERROR;
            continue;
        }

        (*count)++;
        if (rec.seq >= *max_seq) {
            *max_seq = rec.seq;
            *max_seq_slot = i;
        }
    }
    return HAL_OK;
}

/* 找到空槽返回OK，确实写满返回BUSY；读失败不能触发回收。 */
static HAL_StatusTypeDef find_writable_slot(void)
{
    uint8_t buf[CTRLLOG_SLOT_SIZE];

    for (uint32_t i = s_next_slot; i < CTRLLOG_SLOT_COUNT; i++) {
        if (storage_flash_read(slot_addr(s_active, i), buf, CTRLLOG_SLOT_SIZE) != HAL_OK)
            return HAL_ERROR;
        if (slot_is_erased(buf)) {
            s_next_slot = i;
            return HAL_OK;
        }
    }
    s_next_slot = CTRLLOG_SLOT_COUNT;
    return HAL_BUSY;
}

/* 去重比较只在同步遍历期间借用调用方数据，不保存其指针。 */
typedef struct {
    uint16_t stream, len;
    uint64_t key;
    const void *payload;
    bool found, conflict;
} dedup_query_t;

/* 相同键必须同时匹配完整内容，冲突不能被另一条匹配记录掩盖。 */
static void dedup_visit(const ctrllog_record_t *rec, void *user)
{
    dedup_query_t *query = user;
    if (rec->stream != query->stream || rec->dedup_key != query->key) return;
    query->found = true;
    if (rec->payload_len != query->len ||
        (query->len != 0U && memcmp(rec->payload, query->payload, query->len) != 0))
        query->conflict = true;
}

/* ---------- 对外 API ---------- */

HAL_StatusTypeDef storage_ctrllog_init(void)
{
    s_erase_offset = 0U;
    uint64_t seq0 = 0, seq1 = 0;
    uint32_t slot0 = 0, slot1 = 0;
    uint32_t cnt0 = 0, cnt1 = 0;

    s_inited   = false;
    s_erasing  = -1;
    s_last_seq = 0;
    s_next_slot = 0;

    if (scan_zone(0, &cnt0, &seq0, &slot0) != HAL_OK) return HAL_ERROR;
    if (scan_zone(1, &cnt1, &seq1, &slot1) != HAL_OK) return HAL_ERROR;

    if (cnt0 == 0 && cnt1 == 0) {
        s_active = 0;
        s_next_slot = 0;
    } else if (cnt1 == 0) {
        s_active = 0;
        s_next_slot = slot0 + 1;
        s_last_seq = seq0;
    } else if (cnt0 == 0) {
        s_active = 1;
        s_next_slot = slot1 + 1;
        s_last_seq = seq1;
    } else {
        s_active = (seq0 >= seq1) ? 0 : 1;
        s_next_slot = (s_active == 0 ? slot0 : slot1) + 1;
        s_last_seq = (s_active == 0 ? seq0 : seq1);
    }

    s_inited = true;
    return HAL_OK;
}

static HAL_StatusTypeDef storage_ctrllog_append_impl(uint16_t stream, uint64_t dedup_key,
                                         const void *payload, uint16_t len,
                                         bool *is_duplicate)
{
    if (is_duplicate != NULL) *is_duplicate = false;

    if (!s_inited) return HAL_ERROR;
    if (len > CTRLLOG_PAYLOAD_MAX) return HAL_ERROR;
    if (len != 0 && payload == NULL) return HAL_ERROR;

    /* 回收期间不写 */
    if (s_erasing >= 0) return HAL_BUSY;

    /* 去重 */
    if (dedup_key != 0U) {
        dedup_query_t query = { stream, len, dedup_key, payload, false, false };
        HAL_StatusTypeDef status = storage_ctrllog_foreach(dedup_visit, &query);
        if (status != HAL_OK || query.conflict) return HAL_ERROR;
        if (query.found) {
            if (is_duplicate != NULL) *is_duplicate = true;
            return HAL_OK;
        }
    }

    /* 不知道记录是否过期及可靠输出时，不能擦除已提交记录。 */
    HAL_StatusTypeDef space = find_writable_slot();
    if (space != HAL_OK) {
        if (space != HAL_BUSY) return space;
        uint32_t count, last_slot;
        uint64_t last_seq;
        if (scan_zone(1 - s_active, &count, &last_seq, &last_slot) != HAL_OK) return HAL_ERROR;
        if (count != 0U) return HAL_BUSY;
        s_erase_offset = 0U;
        s_erasing = 1 - s_active;
        return HAL_BUSY;
    }

    uint64_t seq;
    if (storage_seq_alloc(&seq) != HAL_OK) return HAL_ERROR;

    uint8_t slot[CTRLLOG_SLOT_SIZE];
    memset(slot, 0xFF, sizeof(slot));

    put_u32(slot + OFF_MAGIC,   CTRLLOG_MAGIC);
    put_u16(slot + OFF_VERSION, CTRLLOG_FORMAT_VERSION);
    put_u16(slot + OFF_STREAM,  stream);
    put_u16(slot + OFF_LEN,     len);
    put_u16(slot + OFF_FLAGS,   0);
    put_u32(slot + OFF_TS,      storage_get_unix_time());
    put_u64(slot + OFF_SEQ,     seq);
    put_u64(slot + OFF_KEY,     dedup_key);
    if (len != 0) memcpy(slot + OFF_PAYLOAD, payload, len);

    uint32_t crc_off = CTRLLOG_HEADER_SIZE + len;
    put_u32(slot + crc_off, crc32_calc(slot, crc_off));

    uint32_t addr = slot_addr(s_active, s_next_slot);

    /* 1) 写内容（header + payload + crc） */
    if (storage_flash_write(addr, slot, crc_off + sizeof(uint32_t)) != HAL_OK)
        return HAL_ERROR;

    /* 2) 最后写提交尾标 */
    uint32_t commit = CTRLLOG_COMMIT;
    if (storage_flash_write(addr + CTRLLOG_COMMIT_OFF,
                                (const uint8_t *)&commit, sizeof(commit)) != HAL_OK)
        return HAL_ERROR;

    /* 3) 回读校验 */
    ctrllog_record_t verify;
    if (storage_flash_read(addr, slot, sizeof(slot)) != HAL_OK || !parse_slot(slot, &verify) ||
        verify.seq != seq || verify.stream != stream || verify.dedup_key != dedup_key ||
        verify.payload_len != len || (len != 0U && memcmp(verify.payload, payload, len) != 0))
        return HAL_ERROR;

    s_next_slot++;
    s_last_seq = seq;
    return HAL_OK;
}

HAL_StatusTypeDef storage_ctrllog_foreach(ctrllog_visit_fn fn, void *user)
{
    if (!s_inited) return HAL_ERROR;
    if (fn == NULL) return HAL_ERROR;

    int order[2] = { 1 - s_active, s_active };
    for (int k = 0; k < 2; k++) {
        for (uint32_t i = 0; i < CTRLLOG_SLOT_COUNT; i++) {
            ctrllog_record_t rec;
            uint8_t buf[CTRLLOG_SLOT_SIZE];
            if (storage_flash_read(slot_addr(order[k], i), buf, sizeof(buf)) != HAL_OK)
                return HAL_ERROR;
            if (!parse_slot(buf, &rec)) {
                if (get_u32(buf + CTRLLOG_COMMIT_OFF) == CTRLLOG_COMMIT) return HAL_ERROR;
                continue;
            }
            fn(&rec, user);
        }
    }
    return HAL_OK;
}

static HAL_StatusTypeDef storage_ctrllog_maintenance_impl(void)
{
    if (s_erasing < 0) return HAL_OK;

    uint32_t base = area_base(s_erasing);
    /* 一轮只擦一个块，给取消、监督和其他队列留出处理机会。 */
    if (storage_flash_erase64(base + s_erase_offset) != HAL_OK)
        return HAL_ERROR;
    s_erase_offset += 0x10000U;
    if (s_erase_offset < FLASH_CTRLLOG_AREA_SIZE) return HAL_BUSY;
    s_erase_offset = 0U;

    s_active = s_erasing;
    s_next_slot = 0;
    s_erasing = -1;
    return HAL_OK;
}

uint64_t storage_ctrllog_last_seq(void)
{
    return s_last_seq;
}

/* 追加带去重键的控制记录，完整提交后才更新索引。 */
HAL_StatusTypeDef storage_ctrllog_append(uint16_t stream, uint64_t dedup_key, const void *payload, uint16_t len, bool *is_duplicate)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_ctrllog_append_impl(stream, dedup_key, payload, len, is_duplicate);
    storage_mutation_end();
    return st;
}

/* 推进一个回收块，未完成时保留回收状态。 */
HAL_StatusTypeDef storage_ctrllog_maintenance(void)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_ctrllog_maintenance_impl();
    storage_mutation_end();
    return st;
}

/* 已启动但尚未完成的整区回收不能被升级安装截断。 */
bool storage_ctrllog_is_busy(void)
{
    return s_erasing >= 0;
}
