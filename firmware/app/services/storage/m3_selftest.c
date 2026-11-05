#include "m3_selftest.h"

#if M3_SELFTEST_ENABLE

#include <stdio.h>
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

#define SELF_JEDEC_ID   0xEF4018U   /* W25Q128：厂家 0xEF + 容量 0x18 */
#define SELF_MIN_YEAR   2024U       /* 低于此年视为 RTC 未校时 */

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

void m3_selftest_run(void)
{
    char detail[96];

    s_pass = s_warn = s_fail = 0;

    printf("\r\n===== M3 SELF TEST =====\r\n");

    /* 1. SPI Flash：JEDEC ID 必须是 0xEF4018 */
    {
        uint32_t id = spi_flash_w25q128_read_id();

        snprintf(detail, sizeof(detail), "jedec=%06lX", (unsigned long)id);
        judge(id == SELF_JEDEC_ID ? SELF_PASS : SELF_FAIL, "SPI Flash", detail);
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
                 "ready=%d pending=%lu dropped=%lu last_seq=%llu",
                 ready ? 1 : 0,
                 (unsigned long)pending,
                 (unsigned long)dropped,
                 (unsigned long long)storage_record_last_seq());
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

    /* 5. 序列号高水位 */
    {
        uint64_t wm = storage_seq_get_high_watermark();

        snprintf(detail, sizeof(detail), "high_watermark=%llu",
                 (unsigned long long)wm);
        judge(wm >= SEQ_FIRST_VALUE ? SELF_PASS : SELF_FAIL, "Seq", detail);
    }

    /* 6. 持久时间下界（0 表示尚无参考值，首次上电正常） */
    {
        uint32_t floor_v = storage_timefloor_get();
        uint32_t now     = storage_get_unix_time();

        snprintf(detail, sizeof(detail), "floor=%lu trusted=%d",
                 (unsigned long)floor_v, storage_timefloor_trusted(now) ? 1 : 0);
        judge(floor_v == 0 ? SELF_WARN : SELF_PASS, "TimeFloor", detail);
    }

    /* 7. 控制日志：遍历双区统计可读记录数 */
    {
        uint32_t n = 0;
        HAL_StatusTypeDef st = storage_ctrllog_foreach(ctrllog_count_cb, &n);

        snprintf(detail, sizeof(detail), "records=%lu scan=%s",
                 (unsigned long)n, (st == HAL_OK) ? "ok" : "err");
        judge(st == HAL_OK ? SELF_PASS : SELF_FAIL, "CtrlLog", detail);
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
