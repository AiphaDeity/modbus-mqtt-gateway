/*
 * storage_ctrllog.c — 控制日志（双区定长槽 + 整区回收）
 *
 * 详见 storage_ctrllog.h 的槽格式说明。
 *
 * 关键约束：
 *   - 写槽顺序：先写 header+payload+crc，最后单独写提交尾标 commit。
 *     掉电中断时 commit 不完整，读取方因 commit/crc 校验失败自动丢弃半成品。
 *   - 双区：当前区写满后，回收另一区（64KB 块擦除 × 8），再切换过去。
 *     回收在 maintenance() 中推进，期间 append 返回 HAL_BUSY。
 *   - 去重：内存中保留最近 128 条记录的 (stream, dedup_key)，启动时从
 *     两个区重放恢复；因此重启后仍能抑制重复执行。
 */

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
#define CTRLLOG_DEDUP_KEEP  128U

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
static int      s_erasing  = -1;  /* >=0 表示正在回收该区 */

/* 去重索引（最近 128 条） */
static uint64_t s_key[CTRLLOG_DEDUP_KEEP];
static uint16_t s_key_stream[CTRLLOG_DEDUP_KEEP];
static uint32_t s_key_count = 0;

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

/* 读取一个槽并解析 */
static bool slot_read_valid(int zone, uint32_t index, ctrllog_record_t *rec)
{
    uint8_t buf[CTRLLOG_SLOT_SIZE];
    if (spi_flash_w25q128_read(slot_addr(zone, index), buf, CTRLLOG_SLOT_SIZE) != HAL_OK)
        return false;
    return parse_slot(buf, rec);
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
        if (spi_flash_w25q128_read(slot_addr(zone, i), buf, CTRLLOG_SLOT_SIZE) != HAL_OK)
            return HAL_ERROR;

        ctrllog_record_t rec;
        if (!parse_slot(buf, &rec)) continue;

        (*count)++;
        if (rec.seq >= *max_seq) {
            *max_seq = rec.seq;
            *max_seq_slot = i;
        }
    }
    return HAL_OK;
}

/* 从 s_next_slot 起找到第一个已擦除槽 */
static bool find_writable_slot(void)
{
    uint8_t buf[CTRLLOG_SLOT_SIZE];

    for (uint32_t i = s_next_slot; i < CTRLLOG_SLOT_COUNT; i++) {
        if (spi_flash_w25q128_read(slot_addr(s_active, i), buf, CTRLLOG_SLOT_SIZE) != HAL_OK)
            return false;
        if (slot_is_erased(buf)) {
            s_next_slot = i;
            return true;
        }
    }
    s_next_slot = CTRLLOG_SLOT_COUNT;
    return false;
}

/* ---------- 去重索引 ---------- */

static void dedup_reset(void)
{
    memset(s_key, 0, sizeof(s_key));
    memset(s_key_stream, 0, sizeof(s_key_stream));
    s_key_count = 0;
}

static bool dedup_contains(uint16_t stream, uint64_t key)
{
    if (key == 0) return false;
    for (uint32_t i = 0; i < CTRLLOG_DEDUP_KEEP; i++) {
        if (s_key[i] == key && s_key_stream[i] == stream) return true;
    }
    return false;
}

static void dedup_insert(uint16_t stream, uint64_t key)
{
    if (key == 0) return;
    uint32_t pos = s_key_count % CTRLLOG_DEDUP_KEEP;
    s_key[pos] = key;
    s_key_stream[pos] = stream;
    s_key_count++;
}

/* ---------- 对外 API ---------- */

HAL_StatusTypeDef storage_ctrllog_init(void)
{
    uint64_t seq0 = 0, seq1 = 0;
    uint32_t slot0 = 0, slot1 = 0;
    uint32_t cnt0 = 0, cnt1 = 0;

    s_inited   = false;
    s_erasing  = -1;
    s_last_seq = 0;
    s_next_slot = 0;
    dedup_reset();

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

    /* 重放去重键：旧区先、当前区后，保证"最近优先" */
    int order[2] = { 1 - s_active, s_active };
    for (int k = 0; k < 2; k++) {
        for (uint32_t i = 0; i < CTRLLOG_SLOT_COUNT; i++) {
            ctrllog_record_t rec;
            uint8_t buf[CTRLLOG_SLOT_SIZE];
            if (spi_flash_w25q128_read(slot_addr(order[k], i), buf, CTRLLOG_SLOT_SIZE) != HAL_OK)
                return HAL_ERROR;
            if (!parse_slot(buf, &rec)) continue;
            dedup_insert(rec.stream, rec.dedup_key);
        }
    }

    s_inited = true;
    return HAL_OK;
}

HAL_StatusTypeDef storage_ctrllog_append(uint16_t stream, uint64_t dedup_key,
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
    if (dedup_contains(stream, dedup_key)) {
        if (is_duplicate != NULL) *is_duplicate = true;
        return HAL_OK;
    }

    /* 当前区写满则启动回收，本次返回忙 */
    if (s_next_slot >= CTRLLOG_SLOT_COUNT || !find_writable_slot()) {
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
    if (spi_flash_w25q128_write(addr, slot, crc_off + sizeof(uint32_t)) != HAL_OK)
        return HAL_ERROR;

    /* 2) 最后写提交尾标 */
    uint32_t commit = CTRLLOG_COMMIT;
    if (spi_flash_w25q128_write(addr + CTRLLOG_COMMIT_OFF,
                                (const uint8_t *)&commit, sizeof(commit)) != HAL_OK)
        return HAL_ERROR;

    /* 3) 回读校验 */
    ctrllog_record_t verify;
    if (!slot_read_valid(s_active, s_next_slot, &verify) || verify.seq != seq)
        return HAL_ERROR;

    s_next_slot++;
    s_last_seq = seq;
    dedup_insert(stream, dedup_key);
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
            if (!slot_read_valid(order[k], i, &rec)) continue;
            fn(&rec, user);
        }
    }
    return HAL_OK;
}

HAL_StatusTypeDef storage_ctrllog_maintenance(void)
{
    if (s_erasing < 0) return HAL_OK;

    uint32_t base = area_base(s_erasing);
    for (uint32_t off = 0; off < FLASH_CTRLLOG_AREA_SIZE; off += 0x10000U) {
        if (spi_flash_w25q128_block64_erase(base + off) != HAL_OK)
            return HAL_ERROR;   /* 保留状态，下次重试 */
    }

    s_active = s_erasing;
    s_next_slot = 0;
    s_erasing = -1;
    return HAL_OK;
}

uint64_t storage_ctrllog_last_seq(void)
{
    return s_last_seq;
}
