#ifndef STORAGE_RECORD_H
#define STORAGE_RECORD_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * TF 卡记录持久化（FatFs，段文件 4MiB × 48 个，共 192MiB 环形覆盖）。
 *
 * 对齐《M2→M3 存储接口约定 v0.1》：
 *   - M2 是记录生产者（离线缓存），联网后拉取补传，按 ack 水位删除
 *   - 记录语义字段：kind / seq / captured_at_ms / payload(JSON 原文)
 *   - M3 内部只用 (kind, seq) 定位；record_id 字符串由 M2 补传时拼装
 *
 * 壳格式（M3 内部实现，M2 无感；语义与 M2 约定一致）：
 *   - 段文件 /records/seg_0000.bin ~ seg_0047.bin，832B 定长槽
 *   - 定长槽 + CRC32 + 提交尾标：掉电最多留半成品（无尾标），扫槽时跳过
 *   - 环状态（cur_seg/next_slot/ack水位/dropped）持久化在 W25Q128 双副本，
 *     上电免全盘扫描（原实现开机要读 192MiB，不可接受）
 *
 * 记录槽（832B，小端）：
 *   偏移 0    magic(4)  = 'FRMT'
 *   偏移 4    format_version(2)
 *   偏移 6    kind(2)              1=telemetry 2=event
 *   偏移 8    payload_len(2)       ≤768
 *   偏移 10   flags(2)
 *   偏移 12   captured_at_ms(8)    Unix 毫秒；未校时 = 0
 *   偏移 20   seq(8)               uint64 单调递增（M2 经 storage_seq 分配）
 *   偏移 28   reserved(4)
 *   偏移 32   payload(payload_len) JSON 原文 UTF-8，最大 768B
 *   偏移 32+len  crc32(4)          覆盖 [0, 32+len)
 *   偏移 828  commit(4) = 'RCDT'   最后写入
 */

#define RECORD_FORMAT_VERSION    3U  /* v3: PAYLOAD_MAX 512→768（16点完整报文） */
#define RECORD_SLOT_SIZE         832U  /* 32B头 + 768B正文 + 4B CRC + 4B提交尾标 + 24B余量 */
#define RECORD_PAYLOAD_MAX       768U
#define RECORD_SEGMENT_SIZE      (4U * 1024U * 1024U)  /* 4 MiB */
#define RECORD_SEGMENT_SLOTS     (RECORD_SEGMENT_SIZE / RECORD_SLOT_SIZE)
#define RECORD_SEGMENT_COUNT     48U

/* kind 定义（M2 约定 3.2 节） */
#define RECORD_KIND_TELEMETRY    1U
#define RECORD_KIND_EVENT        2U

/* 初始 ack 水位 = 首发 seq - 1。M2 约定全局 seq 从 1001 起（见 SEQ_FIRST_VALUE），
 * 用 1000 作基准，”无记录“时 pending/last_seq 都等于它，pending 才不会虚高。 */
#define RECORD_SEQ_INITIAL_WM    1000ULL

/* 单条取出的记录（补传应答用） */
typedef struct {
    uint8_t  kind;
    uint64_t seq;
    uint64_t captured_at_ms;
    uint16_t payload_len;
    uint8_t  payload[RECORD_PAYLOAD_MAX];
} record_out_t;

/* 遍历用视图（payload 指向内部缓冲，仅在回调内有效） */
typedef struct {
    uint16_t        kind;
    uint64_t        seq;
    uint64_t        captured_at_ms;
    uint16_t        payload_len;
    const uint8_t  *payload;
} record_view_t;

typedef void (*record_visit_fn)(const record_view_t *rec, void *user);

/* 正常业务初始化：挂载现有卷并恢复记录环，不因读失败自动格式化。 */
HAL_StatusTypeDef storage_record_init(void);
/* 试运行只挂载现有卷、检查环状态及当前文件；不创建、格式化或写回。 */
HAL_StatusTypeDef storage_record_restore(void);
/* Storage_Task结算同步：f_sync与环状态写入/读回均完成才成功。 */
HAL_StatusTypeDef storage_record_sync(void);

/*
 * 追加一条记录。seq 由调用方（M2 经 storage_seq_alloc）分配并保证单调；
 * captured_at_ms 未校时传 0；payload 为 JSON 原文，≤RECORD_PAYLOAD_MAX。
 * 段写满自动切下一段并回收最旧段（未 ack 的计入 dropped）。
 */
HAL_StatusTypeDef storage_record_append(uint8_t kind, uint64_t seq,
                                        uint64_t captured_at_ms,
                                        const void *payload, uint16_t len);

// 调用方须逐条核验待发前缀均已业务确认；不能凭乱序回执的最大seq推进。节流持久化。
void storage_record_ack_upto(uint64_t ack_upto_seq);

/*
 * 补传拉取：按环序（旧→新）取最多 max_count 条 seq > ack 水位的记录。
 * out 由调用方提供（M2 侧队列应答缓冲），*count 返回实际条数（0 = 无历史）。
 */
HAL_StatusTypeDef storage_record_fetch(uint16_t max_count,
                                       record_out_t *out, uint16_t *count);

/* 缓存统计：pending = 未补传条数（近似，seq 号段空洞会略偏高）；dropped = 未 ack 即被覆盖/写失败的条数 */
void storage_record_stats(uint32_t *pending, uint32_t *dropped);

/* 按写入顺序（旧→新）遍历所有有效记录（调试/测试用） */
HAL_StatusTypeDef storage_record_foreach(record_visit_fn fn, void *user);

/* 最近一条已提交记录的序列号（无记录时返回 RECORD_SEQ_INITIAL_WM，即 1000） */
uint64_t storage_record_last_seq(void);

/* 周期维护：环状态节流落盘（Storage_Task 循环调用） */
void storage_record_maintenance(void);

/* 是否已就绪（卡挂载成功） */
bool storage_record_is_ready(void);

/* 介质变化时失效文件对象，不向旧卡补写；保留尚未持久的RAM状态。 */
void storage_record_unmount(void);

#endif /* STORAGE_RECORD_H */
