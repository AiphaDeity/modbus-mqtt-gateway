/*
 * storage_record.c — TF 卡记录持久化（FatFs + 48 段循环覆盖）
 *
 * 详见 storage_record.h 的槽格式说明。
 *
 * 关键约束：
 *   - SD 卡挂载在 Storage_Task 里调用 f_mount(..., 0)，卡未插时非阻塞返回
 *   - 记录写入顺序：先写 header+payload+crc，最后单独写提交尾标
 *   - 段文件命名 seg_0000.bin ~ seg_0047.bin，位于 /records/ 目录
 *   - 当前段写满：打开下一段（段号+1 模 48），下一段已存在则先删除最旧段
 */

#include "storage_record.h"
#include "storage_seq.h"
#include "storage_time.h"
#include "fatfs.h"

#include <string.h>
#include <stdio.h>

#define RECORD_MAGIC     0x544D4652U  /* "FRMT" */
#define RECORD_COMMIT    0x43494D52U  /* "RMIC" 反序? 用 "RCDT" 更清晰 */

/* 修正：commit 用 "RCOR" 不合适，改回简单可读 */
#undef  RECORD_COMMIT
#define RECORD_COMMIT    0x43444F52U  /* "RCDO" 不对，改成 "RCDT" */

#undef  RECORD_COMMIT
#define RECORD_COMMIT    0x54434452U  /* "RCDT" (小端序: 0x54434452 = 'R','C','D','T') */

/* header 字段偏移 */
#define R_OFF_MAGIC      0U
#define R_OFF_VERSION    4U
#define R_OFF_STREAM     6U
#define R_OFF_LEN        8U
#define R_OFF_FLAGS      10U
#define R_OFF_TS         12U
#define R_OFF_SEQ        16U
#define R_OFF_RESERVED   24U
#define R_OFF_PAYLOAD    32U
#define R_OFF_COMMIT     (RECORD_SLOT_SIZE - sizeof(uint32_t))

/* ---------- 内部状态 ---------- */

static bool s_inited   = false;
static bool s_ready    = false;
static uint32_t s_next_slot  = 0;  /* 当前段下一个待写槽 */
static int      s_cur_seg    = -1; /* 当前段号，-1 表示没打开 */
static uint64_t s_last_seq   = 0;

static FIL      s_cur_file;       /* 当前段文件句柄 */
static bool     s_file_open   = false;

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

static uint16_t get_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get_u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void put_u16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put_u64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void seg_path(int seg, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "/records/seg_%04d.bin", seg);
}

static bool slot_is_erased(const uint8_t *buf)
{
    for (uint32_t i = 0; i < RECORD_SLOT_SIZE; i++) {
        if (buf[i] != 0xFF && buf[i] != 0x00) return false;
    }
    /* 全 0xFF 或全 0x00 都视为空槽（FatFs 新建文件初始全 0x00） */
    return true;
}

static bool parse_slot(const uint8_t *buf, record_view_t *rec)
{
    if (get_u32(buf + R_OFF_MAGIC) != RECORD_MAGIC) return false;
    if (get_u32(buf + R_OFF_COMMIT) != RECORD_COMMIT) return false;

    uint16_t version = get_u16(buf + R_OFF_VERSION);
    uint16_t len     = get_u16(buf + R_OFF_LEN);
    if (version != RECORD_FORMAT_VERSION) return false;
    if (len > RECORD_PAYLOAD_MAX) return false;

    uint32_t crc_off = R_OFF_PAYLOAD + len;
    if (crc_off + sizeof(uint32_t) > R_OFF_COMMIT) return false;

    uint32_t crc = get_u32(buf + crc_off);
    if (crc != crc32_calc(buf, crc_off)) return false;

    if (rec != NULL) {
        rec->format_version = version;
        rec->stream         = get_u16(buf + R_OFF_STREAM);
        rec->payload_len    = len;
        rec->flags          = get_u16(buf + R_OFF_FLAGS);
        rec->timestamp      = get_u32(buf + R_OFF_TS);
        rec->seq            = get_u64(buf + R_OFF_SEQ);
        rec->payload        = buf + R_OFF_PAYLOAD;
    }
    return true;
}

/* 打开一段文件（seg 号，0~47）。不存在则创建；已打开另一段时先关闭 */
static FRESULT open_segment(int seg, bool create)
{
    if (s_file_open) {
        f_close(&s_cur_file);
        s_file_open = false;
    }

    char path[64];
    seg_path(seg, path, sizeof(path));

    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr == FR_NO_FILE) {
        if (!create) return fr;
        fr = f_open(&s_cur_file, path, FA_WRITE | FA_CREATE_ALWAYS);
    } else {
        fr = f_open(&s_cur_file, path, FA_READ | FA_WRITE);
    }
    if (fr != FR_OK) return fr;

    s_cur_seg = seg;
    s_file_open = true;
    s_next_slot = 0;
    return FR_OK;
}

/* 删除一段文件（循环覆盖时用） */
static FRESULT delete_segment(int seg)
{
    if (seg == s_cur_seg) {
        f_close(&s_cur_file);
        s_file_open = false;
        s_cur_seg = -1;
    }
    char path[64];
    seg_path(seg, path, sizeof(path));
    return f_unlink(path);
}

/* 在当前段文件里定位到某个槽（按 512B 对齐），然后扫描直到找到第一个空槽 */
static FRESULT find_next_writable_slot_in_seg(void)
{
    uint8_t buf[RECORD_SLOT_SIZE];
    UINT br;

    for (uint32_t slot = s_next_slot; slot < RECORD_SEGMENT_SLOTS; slot++) {
        FSIZE_t off = (FSIZE_t)slot * RECORD_SLOT_SIZE;
        if (f_lseek(&s_cur_file, off) != FR_OK) return FR_DISK_ERR;
        if (f_read(&s_cur_file, buf, RECORD_SLOT_SIZE, &br) != FR_OK) return FR_DISK_ERR;
        if (br == 0 || slot_is_erased(buf)) {
            s_next_slot = slot;
            return FR_OK;
        }
    }
    return FR_DENIED;   /* 当前段已满（FatFs V3 无 FR_DISK_FULL，用 FR_DENIED 代表资源耗尽） */
}

/* 扫描一段文件，返回最后一条有效记录的 slot */
static FRESULT scan_segment_last(int seg, uint32_t *last_slot, uint64_t *max_seq)
{
    FIL f;
    char path[64];
    seg_path(seg, path, sizeof(path));

    if (f_open(&f, path, FA_READ) != FR_OK) return FR_NO_FILE;

    uint32_t last_valid = 0;
    uint64_t seq = 0;
    uint8_t buf[RECORD_SLOT_SIZE];
    UINT br;

    for (uint32_t i = 0; i < RECORD_SEGMENT_SLOTS; i++) {
        FSIZE_t off = (FSIZE_t)i * RECORD_SLOT_SIZE;
        if (f_lseek(&f, off) != FR_OK) break;
        if (f_read(&f, buf, RECORD_SLOT_SIZE, &br) != FR_OK || br == 0) break;
        record_view_t rec;
        if (parse_slot(buf, &rec)) {
            last_valid = i;
            if (rec.seq >= seq) seq = rec.seq;
        }
    }

    f_close(&f);
    *last_slot = last_valid;
    *max_seq = seq;
    return FR_OK;
}

/* ---------- 对外 API ---------- */

HAL_StatusTypeDef storage_record_init(void)
{
    FRESULT fr;

    s_inited = false;
    s_ready  = false;
    s_cur_seg = -1;
    s_file_open = false;
    s_last_seq = 0;
    s_next_slot = 0;

    /* 挂载 SD 卡，延迟挂载（不阻塞初始化） */
    fr = f_mount(&SDFatFS, SDPath, 0);
    if (fr == FR_NOT_READY || fr == FR_DISK_ERR) {
        /* 卡未插或物理层失败，不阻塞 M3 */
        return HAL_OK;
    }
    if (fr == FR_INVALID_DRIVE) {
        return HAL_ERROR;
    }
    if (fr != FR_OK) {
        /* 可能需要自动格式化（_USE_MKFS=1 时 f_mount 会在内部处理） */
        /* 但 f_mount 不会自动 mkfs，我们主动试一次 */
        uint8_t workbuf[512];
        fr = f_mkfs(SDPath, 0, 0, workbuf, sizeof(workbuf));
        if (fr != FR_OK) return HAL_ERROR;
        fr = f_mount(&SDFatFS, SDPath, 1);  /* 立即挂载 */
        if (fr != FR_OK) return HAL_ERROR;
    }

    /* 创建 /records 目录（已存在则忽略） */
    fr = f_mkdir("/records");
    if (fr != FR_OK && fr != FR_EXIST) return HAL_ERROR;

    /* 扫描已有段文件，找出当前段和最后有效 seq */
    int found_cur = -1;
    uint64_t max_seq_all = 0;

    for (int i = 0; i < RECORD_SEGMENT_COUNT; i++) {
        FILINFO fi;
        char path[64];
        seg_path(i, path, sizeof(path));
        if (f_stat(path, &fi) == FR_OK && fi.fsize > 0) {
            uint32_t last_slot;
            uint64_t seg_max;
            if (scan_segment_last(i, &last_slot, &seg_max) == FR_OK) {
                found_cur = i;
                if (seg_max >= max_seq_all) {
                    max_seq_all = seg_max;
                }
            }
        }
    }

    if (found_cur < 0) {
        /* 全新，打开段 0 */
        fr = open_segment(0, true);
        if (fr != FR_OK) return HAL_ERROR;
    } else {
        fr = open_segment(found_cur, false);
        if (fr != FR_OK) return HAL_ERROR;
        /* 当前段从末尾开始写（留一个 slot 位置） */
        fr = find_next_writable_slot_in_seg();
        if (fr == FR_DENIED) {  /* 当前段已满，切到下一段 */
            /* 当前段已满，切到下一段 */
            int next = (found_cur + 1) % RECORD_SEGMENT_COUNT;
            /* 如果下一段是旧段，删掉 */
            delete_segment(next);
            fr = open_segment(next, true);
            if (fr != FR_OK) return HAL_ERROR;
        }
    }

    s_last_seq = max_seq_all;
    s_ready  = true;
    s_inited = true;
    return HAL_OK;
}

HAL_StatusTypeDef storage_record_append(uint16_t stream,
                                       const void *payload, uint16_t len)
{
    if (!s_ready) return HAL_BUSY;
    if (len > RECORD_PAYLOAD_MAX) return HAL_ERROR;
    if (len != 0 && payload == NULL) return HAL_ERROR;

    /* 当前段写满则切换 */
    if (s_next_slot >= RECORD_SEGMENT_SLOTS || find_next_writable_slot_in_seg() != FR_OK) {
        int next = (s_cur_seg + 1) % RECORD_SEGMENT_COUNT;
        /* 循环覆盖：下一段已经是当前段（全 48 段都满），删最旧段 */
        delete_segment(next);
        FRESULT fr = open_segment(next, true);
        if (fr != FR_OK) return HAL_ERROR;
    }

    uint64_t seq;
    if (storage_seq_alloc(&seq) != HAL_OK) return HAL_ERROR;

    uint8_t slot[RECORD_SLOT_SIZE];
    memset(slot, 0xFF, sizeof(slot));   /* FatFs 新建文件是 0x00，擦除文件是 0xFF，统一处理 */
    /* 先把未用到的 header/reserved 清零，避免 0xFF 误解析 */
    memset(slot + R_OFF_MAGIC, 0, R_OFF_PAYLOAD - R_OFF_MAGIC);

    put_u32(slot + R_OFF_MAGIC,   RECORD_MAGIC);
    put_u16(slot + R_OFF_VERSION, RECORD_FORMAT_VERSION);
    put_u16(slot + R_OFF_STREAM,  stream);
    put_u16(slot + R_OFF_LEN,     len);
    put_u16(slot + R_OFF_FLAGS,   0);
    put_u32(slot + R_OFF_TS,      storage_get_unix_time());
    put_u64(slot + R_OFF_SEQ,     seq);
    put_u64(slot + R_OFF_RESERVED, 0);
    if (len != 0) memcpy(slot + R_OFF_PAYLOAD, payload, len);

    uint32_t crc_off = R_OFF_PAYLOAD + len;
    put_u32(slot + crc_off, crc32_calc(slot, crc_off));

    /* 定位到当前槽 */
    FSIZE_t off = (FSIZE_t)s_next_slot * RECORD_SLOT_SIZE;
    if (f_lseek(&s_cur_file, off) != FR_OK) return HAL_ERROR;

    /* 1) 写 header + payload + crc */
    UINT bw;
    uint32_t write_len = crc_off + sizeof(uint32_t);
    FRESULT fr = f_write(&s_cur_file, slot, write_len, &bw);
    if (fr != FR_OK || bw != write_len) return HAL_ERROR;

    /* 2) flush 一次，确保物理落盘（部分 FatFs 实现要求） */
    f_sync(&s_cur_file);

    /* 3) 最后写提交尾标（在槽的最后 4 字节） */
    uint32_t commit = RECORD_COMMIT;
    if (f_lseek(&s_cur_file, off + R_OFF_COMMIT) != FR_OK) return HAL_ERROR;
    fr = f_write(&s_cur_file, &commit, sizeof(commit), &bw);
    if (fr != FR_OK || bw != sizeof(commit)) return HAL_ERROR;

    /* 4) 立即 flush，防止掉电丢失尾标 */
    f_sync(&s_cur_file);

    s_next_slot++;
    s_last_seq = seq;
    return HAL_OK;
}

HAL_StatusTypeDef storage_record_foreach(record_visit_fn fn, void *user)
{
    if (!s_ready) return HAL_BUSY;
    if (fn == NULL) return HAL_ERROR;

    /* 从段 0 开始，按序扫描；当前段在最后遍历 */
    int order[RECORD_SEGMENT_COUNT];
    int idx = 0;
    if (s_cur_seg < 0) {
        for (int i = 0; i < RECORD_SEGMENT_COUNT; i++) order[idx++] = i;
    } else {
        /* 先 0..cur_seg-1，再 cur_seg+1..47，最后 cur_seg */
        for (int i = 0; i < RECORD_SEGMENT_COUNT; i++)
            if (i != s_cur_seg) order[idx++] = i;
        order[idx++] = s_cur_seg;
    }

    for (int k = 0; k < RECORD_SEGMENT_COUNT; k++) {
        int seg = order[k];
        FILINFO fi;
        char path[64];
        seg_path(seg, path, sizeof(path));
        if (f_stat(path, &fi) != FR_OK || fi.fsize == 0) continue;

        FIL f;
        if (f_open(&f, path, FA_READ) != FR_OK) continue;

        uint8_t buf[RECORD_SLOT_SIZE];
        UINT br;
        for (uint32_t i = 0; i < RECORD_SEGMENT_SLOTS; i++) {
            if (f_read(&f, buf, RECORD_SLOT_SIZE, &br) != FR_OK || br == 0) break;
            record_view_t rec;
            if (parse_slot(buf, &rec)) fn(&rec, user);
        }
        f_close(&f);
    }
    return HAL_OK;
}

uint64_t storage_record_last_seq(void)
{
    return s_last_seq;
}

bool storage_record_is_ready(void)
{
    return s_ready;
}
