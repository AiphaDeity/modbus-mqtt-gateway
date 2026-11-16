/*
 * storage_timefloor.c — 持久时间下界（双份冗余）
 *
 * 记录布局：[magic(4B)][time_floor(4B)][crc32(4B)] = 12 字节
 * 副本位置：0x044000 副本 A，0x045000 副本 B（各占一个 4KiB 扇区）
 *
 * 时间下界单调递增：启动取两份中较大的有效值，写旧副本保新副本。
 */

#include "storage_runtime.h"
#include "storage_timefloor.h"
#include "flash_partition.h"
#include "spi_flash_w25q128.h"
#include <string.h>

#define TIMEFLOOR_MAGIC  0x54464C52U  /* "TFLR" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t time_floor;
    uint32_t crc32;
} timefloor_record_t;

_Static_assert(sizeof(timefloor_record_t) == 12, "timefloor_record_t must be 12 bytes");

#define TIMEFLOOR_CRC_LEN (sizeof(timefloor_record_t) - sizeof(uint32_t))

/* 下界时间的 sanity check：必须 >= 2020-01-01 00:00:00 UTC (1,577,836,800)，
 * 否则即便 CRC/magic 正确也视为无效记录。防止写入截断导致 time_floor=0
 * 或不合理值被当成有效下界。
 */
#define TIMEFLOOR_MIN_VALID_UNIX  1577836800u

static const uint32_t s_copy_addr[2] = { FLASH_TIME_FLOOR_A_ADDR, FLASH_TIME_FLOOR_B_ADDR };

static bool s_read_error;
static bool s_initialized;
static bool s_copy_blank[2];


static uint32_t s_floor = 0;
static int      s_newer = -1;   /* 较大值所在副本索引 */

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

static bool copy_read(int idx, timefloor_record_t *rec)
{
    if (storage_flash_read(s_copy_addr[idx], (uint8_t *)rec,
                               sizeof(*rec)) != HAL_OK) {
        s_read_error = true;
        return false;
    }
    s_copy_blank[idx] = true;
    for (uint32_t i = 0U; i < sizeof(*rec); i++) {
        if (((const uint8_t *)rec)[i] != 0xFFU) s_copy_blank[idx] = false;
    }
    if (rec->magic != TIMEFLOOR_MAGIC) return false;
    if (rec->crc32 != crc32_calc((const uint8_t *)rec, TIMEFLOOR_CRC_LEN)) return false;
    return true;
}

static HAL_StatusTypeDef copy_write(int idx, uint32_t floor)
{
    timefloor_record_t rec;
    rec.magic      = TIMEFLOOR_MAGIC;
    rec.time_floor = floor;
    rec.crc32      = crc32_calc((const uint8_t *)&rec, TIMEFLOOR_CRC_LEN);

    HAL_StatusTypeDef st = storage_flash_erase(s_copy_addr[idx]);
    if (st != HAL_OK) return st;

    return storage_flash_write(s_copy_addr[idx], (const uint8_t *)&rec,
                                    sizeof(rec));
}

HAL_StatusTypeDef storage_timefloor_init(void)
{
    s_initialized = false;
    s_floor = 0U;
    s_newer = -1;
    s_read_error = false;
    s_copy_blank[0] = s_copy_blank[1] = false;
    timefloor_record_t rec_a = {0}, rec_b = {0};
    bool ok_a = copy_read(0, &rec_a);
    bool ok_b = copy_read(1, &rec_b);

    if (s_read_error || (!ok_a && !ok_b && !(s_copy_blank[0] && s_copy_blank[1])))
        return HAL_ERROR;

    s_newer = -1;
    s_floor = 0;

    /* 二次筛选：CRC/magic 通过后，再校验 time_floor 的合理性。
     * 写入截断、非法时间戳都可能产生 "CRC 对但值不合法" 的记录。
     */
    ok_a = ok_a && rec_a.time_floor >= TIMEFLOOR_MIN_VALID_UNIX;
    ok_b = ok_b && rec_b.time_floor >= TIMEFLOOR_MIN_VALID_UNIX;
    if (!ok_a && !ok_b && !(s_copy_blank[0] && s_copy_blank[1])) return HAL_ERROR;

    if (ok_a && ok_b) {
        s_newer = (rec_a.time_floor >= rec_b.time_floor) ? 0 : 1;
    } else if (ok_a) {
        s_newer = 0;
    } else if (ok_b) {
        s_newer = 1;
    }

    if (s_newer >= 0) {
        const timefloor_record_t *rec = (s_newer == 0) ? &rec_a : &rec_b;
        s_floor = rec->time_floor;
    }
    s_initialized = true;
    return HAL_OK;
}

uint32_t storage_timefloor_get(void)
{
    return s_floor;
}

static HAL_StatusTypeDef storage_timefloor_update_impl(uint32_t now)
{
    if (!s_initialized) return HAL_ERROR;
    if (now == 0) return HAL_OK;
    if (now < TIMEFLOOR_MIN_VALID_UNIX) return HAL_ERROR;

    /* 时间倒退：不降低下界，直接忽略 */
    if (now < s_floor) return HAL_OK;

    /* 未达到落盘阈值：内存值不下调也不前进，等下次 */
    if (s_floor != 0 && (now - s_floor) < STORAGE_TIMEFLOOR_STEP_SEC) {
        return HAL_OK;
    }

    /* 覆盖较旧副本（两份都无效时写 A） */
    int target = (s_newer < 0) ? 0 : (1 - s_newer);
    HAL_StatusTypeDef st = copy_write(target, now);
    if (st != HAL_OK) return st;

    /* 回读校验 */
    timefloor_record_t verify;
    if (!copy_read(target, &verify) || verify.time_floor != now) {
        return HAL_ERROR;
    }

    s_floor = now;
    s_newer = target;
    return HAL_OK;
}

bool storage_timefloor_trusted(uint32_t now)
{
    if (!s_initialized) return false;
    if (s_floor == 0) return true;   /* 无参考值（全新设备），不能据此否定 */
    return now >= s_floor;
}

/* 按阈值持久推进时间下界，不降低已保存的值。 */
HAL_StatusTypeDef storage_timefloor_update(uint32_t now)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_timefloor_update_impl(now);
    storage_mutation_end();
    return st;
}
