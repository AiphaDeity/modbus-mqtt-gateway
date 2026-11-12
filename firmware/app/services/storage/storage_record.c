/*
 * storage_record.c — TF 卡记录持久化（FatFs + 48 段循环覆盖）
 *
 * 详见 storage_record.h 的槽格式与 M2 接口约定说明。
 *
 * 关键约束：
 *   - SD 卡挂载在 Storage_Task 里调用 f_mount(..., 0)，卡未插时非阻塞返回
 *   - 记录写入顺序：先写 header+payload+crc，最后单独写提交尾标，各 f_sync
 *   - 段文件 seg_0000.bin ~ seg_0047.bin（/records/），576B 定长槽
 *   - 环状态（cur_seg/next_slot/ack 水位/last_seq/dropped）持久化在
 *     W25Q128 双副本（0x046000/0x047000），上电免全盘扫描；
 *     节流落盘：段切换立即存，平时每 16 条或 30s 存一次
 *   - 删除（ack）只推进水位，不重写文件；段复用时未 ack 记录计入 dropped
 */

#include "storage_runtime.h"
#include "upgrade_quiesce.h"
#include "sdcard_tf.h"
#include "main.h"
#include "storage_record.h"
#include "flash_partition.h"
#include "spi_flash_w25q128.h"
#include "fatfs.h"
#include "diskio.h"

#include <string.h>
#include <stdio.h>

#define RECORD_MAGIC     0x544D4652U  /* "FRMT" 小端序字节 'F','R','M','T' */
#define RECORD_COMMIT    0x54434452U  /* "RCDT" 小端序字节 'R','C','D','T' */

/* header 字段偏移 */
#define R_OFF_MAGIC      0U
#define R_OFF_VERSION    4U
#define R_OFF_KIND       6U
#define R_OFF_LEN        8U
#define R_OFF_FLAGS      10U
#define R_OFF_MS         12U
#define R_OFF_SEQ        20U
#define R_OFF_RESERVED   28U
#define R_OFF_PAYLOAD    32U
#define R_OFF_COMMIT     (RECORD_SLOT_SIZE - sizeof(uint32_t))

/* 环状态落盘节流 */
#define RSTATE_PERSIST_APPENDS   16U
#define RSTATE_PERSIST_MS        30000U
#define RSTATE_MAGIC             0x52535432U  /* "RST2" */

/* ---------- 内部状态 ---------- */

static bool     s_ready   = false;
static uint32_t s_next_slot = 0;   /* 当前段下一个待写槽 */
static int      s_cur_seg   = -1;  /* 当前段号，-1 表示没打开 */
static uint64_t s_last_seq  = 0;   /* 最近一次 append 的 seq（含失败尝试） */
static uint64_t s_ack_wm    = 0;   /* ack 水位：seq ≤ wm 已上传，可复用 */
static uint32_t s_dropped   = 0;   /* 未 ack 即被覆盖/写失败的条数 */
static uint32_t s_gen       = 0;   /* 环状态代数（双副本取较大者） */
static int      s_rstate_newer = 0; /* 当前较新的环状态副本索引(0/1)，由 state_load 恢复 */

/* 补传读游标：下次 fetch 从这里开始，避免每次从最旧段重扫。
 * 语义是「第一条未 ack 记录的位置」——只在 ack 推进到该记录时才前移，
 * 保证 fetch 了但没 ack 的记录下次还能被重新取到。 */
static int      s_fetch_seg  = 0;
static uint32_t s_fetch_slot = 0;

/* 最近一次 fetch 返回的最后一条记录位置（供 ack 推进游标用，仅 RAM） */
static bool     s_last_pos_valid = false;
static int      s_last_pos_seg   = 0;
static uint32_t s_last_pos_slot  = 0;
static uint64_t s_last_pos_seq   = 0;

static FIL      s_cur_file;
static bool     s_file_open = false;
static bool     s_sync_failed;

/* 环状态节流落盘 */
static bool     s_state_dirty  = false;
static uint32_t s_appends_since = 0;
static uint32_t s_last_persist_tick = 0;

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
        rec->kind          = get_u16(buf + R_OFF_KIND);
        rec->payload_len   = len;
        rec->captured_at_ms = get_u64(buf + R_OFF_MS);
        rec->seq           = get_u64(buf + R_OFF_SEQ);
        rec->payload       = buf + R_OFF_PAYLOAD;
    }
    return true;
}

/* 打开一段文件（seg 号，0~47）。不存在时按 create 决定；已打开另一段时先关闭 */
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
    return FR_OK;
}

/* 删除一段文件（段复用前调用） */
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

/* 顺序扫当前文件 [from_slot, SLOTS) 的连续有效前缀，返回真实下一个可写槽与最大 seq */
static FRESULT scan_seg_forward(FIL *f, uint32_t from_slot,
                                uint32_t *next_slot, uint64_t *last_seq)
{
    uint8_t buf[RECORD_SLOT_SIZE];
    UINT br;

    FSIZE_t off = (FSIZE_t)from_slot * RECORD_SLOT_SIZE;
    if (f_lseek(f, off) != FR_OK) return FR_DISK_ERR;

    uint32_t i;
    for (i = from_slot; i < RECORD_SEGMENT_SLOTS; i++) {
        if (f_read(f, buf, RECORD_SLOT_SIZE, &br) != FR_OK) return FR_DISK_ERR;
        if (br < RECORD_SLOT_SIZE) break;          /* EOF：未写满的尾部 */
        record_view_t rec;
        if (!parse_slot(buf, &rec)) break;         /* 第一个无效槽即结束 */
        if (rec.seq > *last_seq) *last_seq = rec.seq;
    }
    *next_slot = i;
    return FR_OK;
}

/* 扫描一段文件（fallback 全盘恢复用），返回最后有效槽号与段内最大 seq */
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

/* 统计某段中尚未 ack（seq > 水位）的有效记录数；段复用回收前调用 */
static uint32_t count_unacked_in_segment(int seg)
{
    FIL f;
    char path[64];
    seg_path(seg, path, sizeof(path));

    if (f_open(&f, path, FA_READ) != FR_OK) return 0;

    uint8_t buf[RECORD_SLOT_SIZE];
    UINT br;
    uint32_t n = 0;

    for (uint32_t i = 0; i < RECORD_SEGMENT_SLOTS; i++) {
        if (f_read(&f, buf, RECORD_SLOT_SIZE, &br) != FR_OK || br == 0) break;
        if (br < RECORD_SLOT_SIZE) break;
        record_view_t rec;
        if (parse_slot(buf, &rec) && rec.seq > s_ack_wm) n++;
    }

    f_close(&f);
    return n;
}

/* ---------- 环状态持久化（W25Q128 双副本） ---------- */
/*
 * 布局（48B，小端）：
 *   0  magic(4) = "RST2"
 *   4  gen(4)           代数，双副本取较大者；写入时写 gen 较旧的那份
 *   8  cur_seg(4)       当前写入段
 *   12 next_slot(4)     当前段下一个待写槽
 *   16 ack_wm(8)        补传水位：seq ≤ 此值已上传
 *   24 last_seq(8)      已写入的最大 seq
 *   32 dropped(4)       未 ack 即被覆盖/写失败的条数
 *   36 fetch_seg(4)     补传读游标：段号（避免每次从最旧段重扫）
 *   40 fetch_slot(4)    补传读游标：槽号
 *   44 crc32(4)         覆盖 [0,44)
 *
 * magic 用 RST2（旧版 RST1 的 40B 布局不兼容，读到即视为无环状态，
 * 退化为全盘扫描一次，不影响正确性）。
 */

typedef struct {
    uint32_t gen;
    int      cur_seg;
    uint32_t next_slot;
    uint64_t ack_wm;
    uint64_t last_seq;
    uint32_t dropped;
    int      fetch_seg;
    uint32_t fetch_slot;
} ring_state_t;

#define RSTATE_SIZE  48U
#define RSTATE_CRC_LEN 44U

static void state_pack(uint8_t *buf, const ring_state_t *st)
{
    memset(buf, 0, RSTATE_SIZE);
    put_u32(buf + 0,  RSTATE_MAGIC);
    put_u32(buf + 4,  st->gen);
    put_u32(buf + 8,  (uint32_t)st->cur_seg);
    put_u32(buf + 12, st->next_slot);
    put_u64(buf + 16, st->ack_wm);
    put_u64(buf + 24, st->last_seq);
    put_u32(buf + 32, st->dropped);
    put_u32(buf + 36, (uint32_t)st->fetch_seg);
    put_u32(buf + 40, st->fetch_slot);
    put_u32(buf + 44, crc32_calc(buf, RSTATE_CRC_LEN));
}

static bool state_unpack(const uint8_t *buf, ring_state_t *st)
{
    if (get_u32(buf + 0) != RSTATE_MAGIC) return false;
    if (get_u32(buf + 44) != crc32_calc(buf, RSTATE_CRC_LEN)) return false;

    uint32_t cs = get_u32(buf + 8);
    uint32_t fs = get_u32(buf + 36);
    if (cs >= RECORD_SEGMENT_COUNT) return false;
    if (fs >= RECORD_SEGMENT_COUNT) return false;

    st->gen        = get_u32(buf + 4);
    st->cur_seg    = (int)cs;
    st->next_slot  = get_u32(buf + 12);
    st->ack_wm     = get_u64(buf + 16);
    st->last_seq   = get_u64(buf + 24);
    st->dropped    = get_u32(buf + 32);
    st->fetch_seg  = (int)fs;
    st->fetch_slot = get_u32(buf + 40);

    if (st->next_slot > RECORD_SEGMENT_SLOTS) return false;
    if (st->fetch_slot > RECORD_SEGMENT_SLOTS) return false;
    return true;
}

/* 立即落盘环状态（写 gen 较旧的副本） */
static HAL_StatusTypeDef state_store_now(void)
{
    if (s_gen == UINT32_MAX || s_cur_seg < 0) return HAL_ERROR;
    ring_state_t state = {
        .gen = s_gen + 1U, .cur_seg = s_cur_seg, .next_slot = s_next_slot,
        .ack_wm = s_ack_wm, .last_seq = s_last_seq, .dropped = s_dropped,
        .fetch_seg = s_fetch_seg, .fetch_slot = s_fetch_slot
    };
    uint8_t data[RSTATE_SIZE], check[RSTATE_SIZE];
    state_pack(data, &state);
    const uint32_t addr[2] = {FLASH_RING_STATE_A_ADDR, FLASH_RING_STATE_B_ADDR};
    int target = s_rstate_newer == 0 ? 1 : 0;
    HAL_StatusTypeDef st = storage_flash_erase(addr[target]);
    if (st == HAL_OK) st = storage_flash_write(addr[target], data, sizeof(data));
    if (st == HAL_OK) st = storage_flash_read(addr[target], check, sizeof(check));
    if (st == HAL_OK && memcmp(data, check, sizeof(data)) != 0) st = HAL_ERROR;
    if (st == HAL_OK) {
        s_rstate_newer = target;
        s_gen = state.gen;
        s_state_dirty = false;
        s_appends_since = 0U;
    }
    s_last_persist_tick = HAL_GetTick();
    return st;
}

/* 上电读环状态：双副本取 gen 较大的有效份 */
static HAL_StatusTypeDef state_load(ring_state_t *out, bool *found)
{
    const uint32_t addr[2] = {FLASH_RING_STATE_A_ADDR, FLASH_RING_STATE_B_ADDR};
    uint8_t data[RSTATE_SIZE];
    ring_state_t copies[2];
    bool valid[2] = {false, false};
    bool blank[2] = {true, true};
    *found = false;
    for (int i = 0; i < 2; i++) {
        HAL_StatusTypeDef st = storage_flash_read(addr[i], data, sizeof(data));
        if (st != HAL_OK) return st;
        for (uint32_t j = 0U; j < sizeof(data); j++)
            if (data[j] != 0xFFU) blank[i] = false;
        valid[i] = state_unpack(data, &copies[i]);
    }
    if (!valid[0] && !valid[1]) return blank[0] && blank[1] ? HAL_OK : HAL_ERROR;
    int selected = !valid[0] || (valid[1] && copies[1].gen > copies[0].gen) ? 1 : 0;
    *out = copies[selected];
    s_rstate_newer = selected;
    *found = true;
    return HAL_OK;
}

/* ---------- 对外 API ---------- */

static HAL_StatusTypeDef storage_record_init_impl(void)
{
    if (s_state_dirty || s_sync_failed) return HAL_BUSY;
    if (s_file_open) {
        if (f_close(&s_cur_file) != FR_OK) return HAL_ERROR;
        s_file_open = false;
    }
    FRESULT fr;

    s_ready   = false;
    s_cur_seg = -1;
    s_file_open = false;
    s_last_seq  = RECORD_SEQ_INITIAL_WM;
    s_next_slot = 0;
    s_ack_wm    = RECORD_SEQ_INITIAL_WM;
    s_dropped   = 0;
    s_gen       = 0;
    s_fetch_seg  = 0;
    s_fetch_slot = 0;
    s_last_pos_valid = false;
    s_state_dirty = false;
    s_appends_since = 0;
    s_last_persist_tick = HAL_GetTick();

    /* 强制物理层重新初始化：
     * 热插拔后 FatFs 里的 fs_type 仍为已挂载，find_volume 会跳过 disk_initialize
     * 直接用旧的 SD 句柄去读，导致 30s 超时。这里先显式初始化一次，
     * 走完整的 BSP_SD_Init（SDIO RCC reset + 重新枚举卡）。
     */
    (void)disk_initialize(0);

    /* 挂载 SD 卡（延迟挂载，不阻塞） */
    fr = f_mount(&SDFatFS, SDPath, 0);
    if (fr == FR_NOT_READY || fr == FR_DISK_ERR) {
        return HAL_OK;   /* 卡未插或物理层失败，不阻塞 M3 */
    }
    if (fr == FR_INVALID_DRIVE) {
        return HAL_ERROR;
    }
    if (fr != FR_OK) return HAL_ERROR;   /* 不因读失败自动格式化介质。 */

    fr = f_mkdir("/records");
    if (fr != FR_OK && fr != FR_EXIST) return HAL_ERROR;

    /* —— 快路径：用 W25Q128 环状态恢复 —— */
    ring_state_t st;

    bool found;
    HAL_StatusTypeDef load = state_load(&st, &found);
    if (load != HAL_OK) return load;
    if (found) {
        char path[64];
        seg_path(st.cur_seg, path, sizeof(path));
        FILINFO fi;
        fr = f_stat(path, &fi);
        if (fr != FR_OK) return HAL_ERROR;
        bool seg_exists = fi.fsize > 0;

        if (seg_exists && open_segment(st.cur_seg, false) == FR_OK) {
            /* 兼容旧环状态（曾从 0 起算）：把水位抬到基准值 */
            if (st.last_seq < RECORD_SEQ_INITIAL_WM) st.last_seq = RECORD_SEQ_INITIAL_WM;
            if (st.ack_wm  < RECORD_SEQ_INITIAL_WM) st.ack_wm  = RECORD_SEQ_INITIAL_WM;

            s_cur_seg   = st.cur_seg;
            s_next_slot = st.next_slot;
            s_ack_wm    = st.ack_wm;
            s_dropped   = st.dropped;
            s_gen       = st.gen;
            s_last_seq  = st.last_seq;
            s_fetch_seg  = st.fetch_seg;
            s_fetch_slot = st.fetch_slot;

            uint32_t ns = 0;
            if (scan_seg_forward(&s_cur_file, s_next_slot, &ns, &s_last_seq) != FR_OK)
                return HAL_ERROR;
            s_next_slot = ns;

            s_ready = true;
            s_state_dirty = true;
            HAL_StatusTypeDef saved = state_store_now();
            if (saved != HAL_OK) s_ready = false;
            return saved;
        }
        /* 段文件丢了 → 落到下面的全盘扫描 */
    }

    /* —— 慢路径：全盘扫描（环状态丢失或首次上电） —— */
    int found_cur = -1;
    uint32_t found_last_slot = 0;
    uint64_t max_seq_all = 0;

    for (int i = 0; i < (int)RECORD_SEGMENT_COUNT; i++) {
        FILINFO fi;
        char path[64];
        seg_path(i, path, sizeof(path));
        if (f_stat(path, &fi) == FR_OK && fi.fsize > 0) {
            uint32_t last_slot;
            uint64_t seg_max;
            if (scan_segment_last(i, &last_slot, &seg_max) == FR_OK && seg_max > max_seq_all) {
                /* seq 全局单调 → 拥有最大 seq 的段就是当前段 */
                max_seq_all = seg_max;
                found_cur = i;
                found_last_slot = last_slot;
            }
        }
    }

    if (found_cur < 0) {
        if (open_segment(0, true) != FR_OK) return HAL_ERROR;
        s_next_slot = 0;
        s_fetch_seg  = 0;
        s_fetch_slot = 0;
    } else {
        if (open_segment(found_cur, false) != FR_OK) return HAL_ERROR;
        s_next_slot = found_last_slot + 1;
        if (s_next_slot > RECORD_SEGMENT_SLOTS) s_next_slot = RECORD_SEGMENT_SLOTS;
        s_last_seq = max_seq_all;
        /* 环状态已丢（ack 水位回到基准）→ 全部记录视为未传，从最旧段开始扫 */
        s_fetch_seg  = (int)((found_cur + 1U) % RECORD_SEGMENT_COUNT);
        s_fetch_slot = 0;
    }

    s_ready = true;
    s_state_dirty = true;
    HAL_StatusTypeDef saved = state_store_now();
    if (saved != HAL_OK) s_ready = false;
    return saved;
}

static HAL_StatusTypeDef storage_record_append_impl(uint8_t kind, uint64_t seq,
                                        uint64_t captured_at_ms,
                                        const void *payload, uint16_t len)
{
    if (!s_ready) return HAL_BUSY;
    if (kind != RECORD_KIND_TELEMETRY && kind != RECORD_KIND_EVENT) return HAL_ERROR;
    if (len > RECORD_PAYLOAD_MAX) return HAL_ERROR;
    if (len != 0 && payload == NULL) return HAL_ERROR;

    /* seq 严格单调；重复/回退的写入视为重复请求，静默忽略 */
    if (seq <= s_last_seq) return HAL_OK;
    /* 结算只追加当前段，不在暂停期间回收段文件；请求留在队列等恢复。 */
    if (s_next_slot >= RECORD_SEGMENT_SLOTS && !UpgradeQuiesce_AllowsMutation())
        return HAL_BUSY;
    /* 记录“已尝试”的高水位（失败也算），保证 pending 统计不漏 */
    s_last_seq = seq;

    /* 当前段写满则切换（循环覆盖最旧段）。
     * s_next_slot 在 init 时已精确定位（环状态快路径 + 段内前向扫描），
     * 运行期顺序追加，无需逐槽探测空位。 */
    if (s_next_slot >= RECORD_SEGMENT_SLOTS) {
        int next = (s_cur_seg + 1) % RECORD_SEGMENT_COUNT;
        s_dropped += count_unacked_in_segment(next);
        (void)delete_segment(next);
        /* 该段内容被覆盖：若读游标停在这里，归零从头扫 */
        if (s_fetch_seg == next) {
            s_fetch_slot     = 0;
            s_last_pos_valid = false;
            s_state_dirty    = true;
        }
        if (open_segment(next, true) != FR_OK) {
            s_state_dirty = true;
            return HAL_ERROR;
        }
        s_next_slot = 0;
        s_state_dirty = true;
        state_store_now();   /* 段切换立即落盘环状态 */
    }

    uint8_t slot[RECORD_SLOT_SIZE];
    memset(slot, 0xFF, sizeof(slot));
    memset(slot + R_OFF_MAGIC, 0, R_OFF_PAYLOAD - R_OFF_MAGIC);

    put_u32(slot + R_OFF_MAGIC,   RECORD_MAGIC);
    put_u16(slot + R_OFF_VERSION, RECORD_FORMAT_VERSION);
    put_u16(slot + R_OFF_KIND,    kind);
    put_u16(slot + R_OFF_LEN,     len);
    put_u16(slot + R_OFF_FLAGS,   0);
    put_u64(slot + R_OFF_MS,      captured_at_ms);
    put_u64(slot + R_OFF_SEQ,     seq);
    put_u32(slot + R_OFF_RESERVED, 0);
    if (len != 0) memcpy(slot + R_OFF_PAYLOAD, payload, len);

    uint32_t crc_off = R_OFF_PAYLOAD + len;
    put_u32(slot + crc_off, crc32_calc(slot, crc_off));

    FSIZE_t off = (FSIZE_t)s_next_slot * RECORD_SLOT_SIZE;
    if (f_lseek(&s_cur_file, off) != FR_OK) {
        s_dropped++;
        s_state_dirty = true;
        return HAL_ERROR;
    }

    /* 1) 写 header + payload + crc */
    UINT bw;
    uint32_t write_len = crc_off + sizeof(uint32_t);
    FRESULT fr = f_write(&s_cur_file, slot, write_len, &bw);
    if (fr != FR_OK || bw != write_len) {
        s_dropped++;
        s_state_dirty = true;
        return HAL_ERROR;
    }
    if (f_sync(&s_cur_file) != FR_OK) {
        s_sync_failed = true;
        s_state_dirty = true;
        return HAL_ERROR;
    }

    /* 2) 最后写提交尾标 */
    uint32_t commit = RECORD_COMMIT;
    if (f_lseek(&s_cur_file, off + R_OFF_COMMIT) != FR_OK) {
        s_dropped++;
        s_state_dirty = true;
        return HAL_ERROR;
    }
    fr = f_write(&s_cur_file, &commit, sizeof(commit), &bw);
    if (fr != FR_OK || bw != sizeof(commit)) {
        s_dropped++;
        s_state_dirty = true;
        return HAL_ERROR;
    }
    if (f_sync(&s_cur_file) != FR_OK) {
        s_sync_failed = true;
        s_state_dirty = true;
        return HAL_ERROR;
    }

    s_next_slot++;
    s_appends_since++;
    s_state_dirty = true;
    return HAL_OK;
}

static void storage_record_ack_upto_impl(uint64_t ack_upto_seq)
{
    if (!s_ready) return;
    if (ack_upto_seq <= s_ack_wm) return;

    s_ack_wm = ack_upto_seq;
    s_state_dirty = true;   /* maintenance 节流落盘 */

    /* 这批记录已确认上传 → 读游标可以安全越过它们，
     * 下次 fetch 不必从这批开头重扫 */
    if (s_last_pos_valid && s_last_pos_seq <= ack_upto_seq) {
        if (s_fetch_seg != s_last_pos_seg || s_fetch_slot != s_last_pos_slot) {
            s_fetch_seg  = s_last_pos_seg;
            s_fetch_slot = s_last_pos_slot;
            if (s_fetch_seg >= (int)RECORD_SEGMENT_COUNT) {
                s_fetch_seg  = 0;
                s_fetch_slot = 0;
            }
            s_state_dirty = true;
        }
        s_last_pos_valid = false;
    }
}

HAL_StatusTypeDef storage_record_fetch(uint16_t max_count,
                                       record_out_t *out, uint16_t *count)
{
    if (!s_ready) return HAL_BUSY;
    if (out == NULL || count == NULL || max_count == 0) return HAL_ERROR;

    *count = 0;
    s_last_pos_valid = false;

    /* 从读游标开始（旧→新环序），而不是每次从最旧段重扫 */
    int seg = s_fetch_seg;
    uint32_t slot = s_fetch_slot;
    if (seg < 0 || seg >= (int)RECORD_SEGMENT_COUNT) { seg = 0; slot = 0; }
    if (slot > RECORD_SEGMENT_SLOTS) slot = 0;

    uint32_t visited = 0;
    bool done = false;

    while (visited < RECORD_SEGMENT_COUNT && !done) {
        FILINFO fi;
        char path[64];
        seg_path(seg, path, sizeof(path));

        if (f_stat(path, &fi) != FR_OK || fi.fsize == 0) {
            seg = (int)((seg + 1U) % RECORD_SEGMENT_COUNT);
            slot = 0;
            visited++;
            continue;
        }

        FIL f;
        if (f_open(&f, path, FA_READ) != FR_OK) {
            seg = (int)((seg + 1U) % RECORD_SEGMENT_COUNT);
            slot = 0;
            visited++;
            continue;
        }

        /* 游标在段内 → 直接 seek 跳过已处理部分，不搬数据 */
        if (slot > 0) {
            if (f_lseek(&f, (FSIZE_t)slot * RECORD_SLOT_SIZE) != FR_OK) {
                f_close(&f);
                break;
            }
        }

        uint8_t buf[RECORD_SLOT_SIZE];
        UINT br;
        while (slot < RECORD_SEGMENT_SLOTS) {
            if (f_read(&f, buf, RECORD_SLOT_SIZE, &br) != FR_OK ||
                br < RECORD_SLOT_SIZE) {
                done = true;      /* 到文件尾 */
                break;
            }
            slot++;

            record_view_t rec;
            if (!parse_slot(buf, &rec)) continue;
            if (rec.seq <= s_ack_wm) continue;   /* 已 ack，跳过 */

            record_out_t *dst = &out[*count];
            dst->kind           = (uint8_t)rec.kind;
            dst->seq            = rec.seq;
            dst->captured_at_ms = rec.captured_at_ms;
            dst->payload_len    = rec.payload_len;
            memcpy(dst->payload, rec.payload, rec.payload_len);
            (*count)++;

            /* 记住这条的位置，等 ack 后再把游标推过来 */
            s_last_pos_valid = true;
            s_last_pos_seg   = seg;
            s_last_pos_slot  = slot;
            s_last_pos_seq   = rec.seq;

            if (*count >= max_count) { done = true; break; }
        }
        f_close(&f);

        if (done) break;

        seg = (int)((seg + 1U) % RECORD_SEGMENT_COUNT);
        slot = 0;
        visited++;
    }

    /* 没有可推进的 ack 时游标原地不动，保证未 ack 数据可重读 */
    return HAL_OK;
}

void storage_record_stats(uint32_t *pending, uint32_t *dropped)
{
    uint32_t p = 0;
    if (s_last_seq > s_ack_wm) {
        p = (uint32_t)(s_last_seq - s_ack_wm);
        /* seq 号段预留空洞会让该值略偏高；掉段（dropped）扣减 */
        p = (p > s_dropped) ? (p - s_dropped) : 0;
    }
    if (pending) *pending = p;
    if (dropped) *dropped = s_dropped;
}

HAL_StatusTypeDef storage_record_foreach(record_visit_fn fn, void *user)
{
    if (!s_ready) return HAL_BUSY;
    if (fn == NULL) return HAL_ERROR;

    for (uint32_t k = 0; k < RECORD_SEGMENT_COUNT; k++) {
        int seg = (s_cur_seg < 0)
                  ? (int)k
                  : (int)((s_cur_seg + 1U + k) % RECORD_SEGMENT_COUNT);

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
            if (br < RECORD_SLOT_SIZE) break;
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

static void storage_record_maintenance_impl(void)
{
    if (!s_ready || !s_state_dirty) return;
    if (s_appends_since >= RSTATE_PERSIST_APPENDS ||
        (HAL_GetTick() - s_last_persist_tick) >= RSTATE_PERSIST_MS) {
        state_store_now();
    }
}

bool storage_record_is_ready(void)
{
    return s_ready;
}

/* ---------- 卸载（热插拔拔出路径） ---------- */

void storage_record_unmount(void)
{
    /* 物理介质已变化，不向新卡或失联卡补写旧FILE缓存。 */
    (void)f_mount(NULL, SDPath, 0);
    s_file_open = false;
    s_ready = false;
    if (!s_state_dirty) s_cur_seg = -1;
    s_last_pos_valid = false;
}

/* 在正常业务阶段恢复记录环，不自动格式化异常介质。 */
HAL_StatusTypeDef storage_record_init(void)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_record_init_impl();
    storage_mutation_end();
    return st;
}

/* 持久保存记录正文及提交尾标，文件同步失败不报成功。 */
HAL_StatusTypeDef storage_record_append(uint8_t kind, uint64_t seq, uint64_t captured_at_ms, const void *payload, uint16_t len)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_record_append_impl(kind, seq, captured_at_ms, payload, len);
    storage_mutation_end();
    return st;
}

/* 登记已确认水位和待保存游标，暂停时保留原状态。 */
void storage_record_ack_upto(uint64_t ack_upto_seq)
{
    if (!storage_mutation_begin()) return;
    storage_record_ack_upto_impl(ack_upto_seq);
    storage_mutation_end();
}

/* 按既定阈值刷新脏环状态，失败保留重试依据。 */
void storage_record_maintenance(void)
{
    if (!storage_mutation_begin()) return;
    storage_record_maintenance_impl();
    storage_mutation_end();
}

/* 只读恢复不建目录、不建段、不迁移、不推进游标或写回环状态。 */
HAL_StatusTypeDef storage_record_restore(void)
{
    s_ready = false;
    if (HAL_GPIO_ReadPin(SD_CARD_DET_GPIO_Port, SD_CARD_DET_Pin) != GPIO_PIN_RESET) return HAL_OK;
    FRESULT fr = f_mount(&SDFatFS, SDPath, 1);
    if (fr == FR_NOT_READY || fr == FR_NO_FILESYSTEM) {
        printf("[M3] TF read-only mount unavailable fr=%u\r\n", (unsigned)fr);
        return HAL_OK;
    }
    if (fr != FR_OK) return HAL_ERROR;
    ring_state_t state;
    bool found;
    s_last_seq = RECORD_SEQ_INITIAL_WM;
    HAL_StatusTypeDef st = state_load(&state, &found);
    if (st != HAL_OK) return st;
    if (!found) {
        for (int i = 0; i < (int)RECORD_SEGMENT_COUNT; i++) {
            char path[64];
            FILINFO info;
            seg_path(i, path, sizeof(path));
            fr = f_stat(path, &info);
            if (fr == FR_OK && info.fsize != 0U) return HAL_ERROR;
            if (fr != FR_OK && fr != FR_NO_FILE && fr != FR_NO_PATH) return HAL_ERROR;
        }
        return HAL_OK;
    }
    s_last_seq = state.last_seq;
    char path[64];
    seg_path(state.cur_seg, path, sizeof(path));
    FIL file;
    fr = f_open(&file, path, FA_READ);
    if (fr != FR_OK) return HAL_ERROR;
    bool valid = f_size(&file) <= RECORD_SEGMENT_SIZE &&
                 f_size(&file) >= (FSIZE_t)state.next_slot * RECORD_SLOT_SIZE;
    if (valid && state.next_slot != 0U) {
        uint8_t slot[RECORD_SLOT_SIZE];
        UINT actual = 0U;
        fr = f_lseek(&file, (FSIZE_t)(state.next_slot - 1U) * RECORD_SLOT_SIZE);
        if (fr == FR_OK) fr = f_read(&file, slot, sizeof(slot), &actual);
        valid = fr == FR_OK && actual == sizeof(slot) && parse_slot(slot, NULL);
    }
    if (f_close(&file) != FR_OK) return HAL_ERROR;
    return valid && !storage_runtime_expired() ? HAL_OK : HAL_ERROR;
}

/* 已暂停生产且没有在途变更后，刷文件及环状态；缺卡且无对象可正常降级。 */
HAL_StatusTypeDef storage_record_sync(void)
{
    if (!storage_runtime_is_owner()) return HAL_BUSY;
    if (s_sync_failed) return HAL_ERROR;
    if (s_file_open && f_sync(&s_cur_file) != FR_OK) return HAL_ERROR;
    if (storage_runtime_expired()) return HAL_TIMEOUT;
    if (s_state_dirty) return state_store_now();
    return HAL_OK;
}
