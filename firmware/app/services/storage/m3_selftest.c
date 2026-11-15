/*
 * M3 上板自检：Storage_Task 起来后跑一次，把 8 项状态打到日志串口。
 *
 * 只做只读检查（序列号和 CtrlLog 两项会真写一次，用来验证写通路和断电保持）。
 *
 * 这里的内容是"给别人看的结论"：每项判断 PASS/WARN/FAIL 并打印依据。
 * 排查用的分层探测（SPI2 状态、PCA9555、深掉电、逐段读 JEDEC）不在本文件，
 * 在 m3_flash_probe.c，由 CMake 的 M3_ENABLE_FLASH_PROBE 控制是否编译，
 * 默认关闭 —— 所以正常读这个文件不会被 200 行诊断代码打断。
 */

#include "m3_selftest.h"

#if M3_SELFTEST_ENABLE

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "rtc_sd3078.h"
#include "spi_flash_w25q128.h"
#include "storage_time.h"
#include "storage_seq.h"
#include "storage_identity.h"
#include "storage_timefloor.h"
#include "storage_ctrllog.h"
#include "storage_record.h"
#include "app_readiness.h"

#if M3_ENABLE_FLASH_PROBE
#include "m3_flash_probe.h"
#endif

#define SELF_MIN_YEAR   2024U       /* 低于此年视为 RTC 未校时 */

/*
 * 关于 64 位数字的打印：
 *   本工程用的是 newlib-nano 的 printf，不支持 %llu —— 会原样打出 "lu"，
 *   日志里看到的 last_seq=lu 就是这个原因，不是值为空。
 *   序列号从 1001 起、每 1024 才落盘一次，一台设备整个生命周期也远不到
 *   2^32，所以这里统一按 32 位打印。真要打满 64 位，得在链接选项里加
 *   -u _printf_long_long，那会增大代码体积，不值得。
 */

enum { SELF_FAIL = 0, SELF_WARN = 1, SELF_PASS = 2 };

static int s_pass;
static int s_warn;
static int s_fail;

static void judge(int level, const char *name, const char *detail)
{
    const char *tag = (level == SELF_PASS) ? "PASS"
                    : (level == SELF_WARN) ? "WARN" : "FAIL";

    if (level == SELF_PASS)      s_pass++;
    else if (level == SELF_WARN) s_warn++;
    else                         s_fail++;

    printf("[%s] %-11s %s\r\n", tag, name, detail ? detail : "");
}

static void ctrllog_count_cb(const ctrllog_record_t *rec, void *user)
{
    (void)rec;
    (*(uint32_t *)user)++;
}

/* 统计带固定测试 payload 的记录数（断电保持验证用） */
#define CTRLLOG_TEST_PAYLOAD  "CTLG_SELFTEST_v1"
static void ctrllog_test_count_cb(const ctrllog_record_t *rec, void *user)
{
    const char *p = CTRLLOG_TEST_PAYLOAD;
    size_t plen = strlen(p);
    if (rec->payload_len == plen && memcmp(rec->payload, p, plen) == 0)
        (*(uint32_t *)user)++;
}

void m3_selftest_run(void)
{
    char detail[96];

    s_pass = s_warn = s_fail = 0;

    printf("\r\n===== M3 SELF TEST =====\r\n");

#if M3_ENABLE_FLASH_PROBE
    /* 排障模式：先跑分层探测（D0~D5），再跑下面的结论项。
     * 打开方式见 m3_flash_probe.h。 */
    m3_flash_probe_run();
#endif

    /* 1. SPI Flash：只读核对W25Q64的JEDEC ID，不代表擦写通路通过。 */
    {
        uint32_t id = spi_flash_w25q128_read_id();

        snprintf(detail, sizeof(detail), "jedec=%06lX", (unsigned long)id);
        judge(id == FLASH_JEDEC_ID ? SELF_PASS : SELF_FAIL, "SPI Flash", detail);
    }

    /* 2. RTC：能读到时间 + 年份合理；未校时只报警告，不算故障 */
    {
        uint32_t now = storage_get_unix_time();
        rtc_datetime_t dt;
        rtc_sd3078_from_unix(now, &dt);

        snprintf(detail, sizeof(detail), "%04u-%02u-%02u %02u:%02u:%02u unix=%lu",
                 (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
                 (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second,
                 (unsigned long)now);

        int lv;
        if (now == 0) {
            lv = SELF_FAIL;                   /* 完全读不到 → I2C 通信问题 */
        } else if (dt.year >= SELF_MIN_YEAR && dt.month >= 1 && dt.month <= 12 &&
                   dt.day >= 1 && dt.day <= 31) {
            lv = SELF_PASS;
        } else {
            lv = SELF_WARN;                   /* 读到了但没校时 */
        }
        judge(lv, "RTC SD3078", detail);
    }

    /* 3. TF 卡 / 记录环状态 */
    {
        bool ready = storage_record_is_ready();
        uint32_t pending = 0, dropped = 0;

        storage_record_stats(&pending, &dropped);
        snprintf(detail, sizeof(detail),
                 "ready=%d pending=%lu dropped=%lu last_seq=%lu",
                 ready ? 1 : 0,
                 (unsigned long)pending,
                 (unsigned long)dropped,
                 (unsigned long)(uint32_t)storage_record_last_seq());
        judge(ready ? SELF_PASS : SELF_FAIL, "TF Record", detail);
    }

    /* 4. 设备身份（双份冗余） */
    {
        char gid[GATEWAY_ID_MAX_LEN + 1] = { 0 };

        if (storage_identity_has_id()) {
            storage_identity_get(gid, sizeof(gid));
            snprintf(detail, sizeof(detail), "id=\"%s\"", gid);
            judge(SELF_PASS, "Identity", detail);
        } else {
            judge(SELF_WARN, "Identity", "unset (first boot is normal)");
        }
    }

    /*
     * 5. 序列号：高水位 + 实际分配一次。
     *    首次 alloc 必然走"窗口用完 → 擦除+写入+回读校验"路径，
     *    这条路径只读高水位测不到，必须真调一次才能暴露写失败。
     */
    {
        uint64_t wm  = storage_seq_get_high_watermark();
        uint64_t seq = 0;
        HAL_StatusTypeDef st = storage_seq_alloc(&seq);

        if (st == HAL_OK) {
            snprintf(detail, sizeof(detail),
                     "high_watermark=%lu alloc=%lu wm_after=%lu",
                     (unsigned long)(uint32_t)wm, (unsigned long)(uint32_t)seq,
                     (unsigned long)(uint32_t)storage_seq_get_high_watermark());
            judge(seq >= wm ? SELF_PASS : SELF_FAIL, "Seq", detail);
        } else {
            snprintf(detail, sizeof(detail),
                     "alloc_failed st=%d high_watermark=%lu",
                     (int)st, (unsigned long)(uint32_t)wm);
            judge(SELF_FAIL, "Seq", detail);
        }
    }

    /*
     * 6. 持久时间下界（0 表示尚无参考值，首次上电正常）
     *
     *    trusted 的语义容易被日志误读：storage_timefloor_trusted() 在
     *    floor==0 时返回 true，因为全新设备没有基准值，不能凭空否定时间。
     *    所以 floor=0 和 trusted=1 同时出现不是矛盾，这里显式标出
     *    "(no-baseline)"，避免看日志的人以为判定写错了。
     */
    {
        uint32_t floor_v = storage_timefloor_get();
        uint32_t now     = storage_get_unix_time();

        snprintf(detail, sizeof(detail), "floor=%lu trusted=%d%s",
                 (unsigned long)floor_v, storage_timefloor_trusted(now) ? 1 : 0,
                 (floor_v == 0) ? "(no-baseline)" : "");
        judge(floor_v == 0 ? SELF_WARN : SELF_PASS, "TimeFloor", detail);
    }

    /*
     * 7. 控制日志：真实写入一次 + 断电保持验证。
     *    用固定 payload 标记测试记录，dedup_key 用当前 boot 分配到的
     *    seq（每次 boot 唯一，避免被去重跳过）。
     *
     *    before  = 本轮写入前已有的测试记录数
     *    after   = 本轮写完后的测试记录数
     *      after == before + 1  → 本轮写入 OK
     *      before >= 1          → 上一轮 boot 写的记录重启后仍在 → 断电保持 OK
     */
    {
        uint32_t before = 0, after = 0;

        storage_ctrllog_foreach(ctrllog_test_count_cb, &before);

        uint64_t my_seq = 0;
        if (storage_seq_alloc(&my_seq) != HAL_OK) {
            snprintf(detail, sizeof(detail), "alloc_seq_failed high_wm=%lu",
                     (unsigned long)(uint32_t)storage_seq_get_high_watermark());
            judge(SELF_FAIL, "CtrlLog", detail);
        } else {
            bool dup = false;
            HAL_StatusTypeDef st = storage_ctrllog_append(
                CTRLLOG_STREAM_GENERIC, my_seq,
                CTRLLOG_TEST_PAYLOAD, strlen(CTRLLOG_TEST_PAYLOAD),
                &dup);

            storage_ctrllog_foreach(ctrllog_test_count_cb, &after);

            uint32_t total = 0;
            storage_ctrllog_foreach(ctrllog_count_cb, &total);

            int level;
            const char *ret_str;
            if (st == HAL_OK && after == before + 1) {
                if (before >= 1) {
                    ret_str = "OK";              /* 上轮 boot 的记录重启后仍在 */
                    level = SELF_PASS;
                } else {
                    ret_str = "write+verify";    /* 首次 boot，写入路径 OK */
                    level = SELF_PASS;
                }
            } else {
                ret_str = (st == HAL_OK) ? "lost_after_write" : "write_failed";
                level = SELF_FAIL;
            }

            snprintf(detail, sizeof(detail),
                     "before=%lu after=%lu retention=%s total=%lu dup=%d",
                     (unsigned long)before, (unsigned long)after,
                     ret_str, (unsigned long)total, dup ? 1 : 0);
            judge(level, "CtrlLog", detail);
        }
    }

    /* 8. 就绪位快照，与 App 的判定口径对齐 */
    {
        uint32_t r = App_GetReadyResources();

        printf("[INFO] readiness=0x%02lX storage=%d time=%d ctrl=%d\r\n",
               (unsigned long)(r & 0xFFU),
               (r & (uint32_t)APP_RESOURCE_STORAGE) ? 1 : 0,
               (r & (uint32_t)APP_RESOURCE_TIME) ? 1 : 0,
               (r & (uint32_t)APP_RESOURCE_CONTROL_STATE) ? 1 : 0);
    }

    printf("===== %d pass / %d warn / %d fail =====\r\n\r\n",
           s_pass, s_warn, s_fail);
}

#endif /* M3_SELFTEST_ENABLE */
