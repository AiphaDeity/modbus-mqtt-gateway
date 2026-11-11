/*
 * storage_identity.c — 设备身份服务（双份冗余）
 *
 * 槽布局：0x040000 副本 A，0x041000 副本 B（每份占一个 4KiB 扇区）
 *
 * 写入时始终覆盖"较旧的那份"，保留较新的一份作为掉电回退。
 */

#include "storage_runtime.h"
#include "storage_identity.h"
#include "flash_partition.h"
#include "spi_flash_w25q128.h"
#include <string.h>

#define IDENTITY_MAGIC    0x4944454EU  /* "IDEN" */
#define IDENTITY_VERSION  1U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    char     gateway_id[GATEWAY_ID_MAX_LEN];
    uint32_t crc32;
} identity_record_t;

_Static_assert(sizeof(identity_record_t) == 48, "identity_record_t must be 48 bytes");

#define IDENTITY_CRC_LEN  (sizeof(identity_record_t) - sizeof(uint32_t))

static const uint32_t s_copy_addr[2] = { FLASH_IDENTITY_A_ADDR, FLASH_IDENTITY_B_ADDR };

static bool s_read_error;
static bool s_copy_blank[2];


static bool     s_valid      = false;
static uint32_t s_generation = 0;
static char     s_id[GATEWAY_ID_MAX_LEN];
static int      s_newer      = -1;   /* 较新副本索引 0/1，-1 表示无有效副本 */

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

/* 读取一个副本，有效则返回 true 并填出记录 */
static bool copy_read(int idx, identity_record_t *rec)
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
    if (rec->magic != IDENTITY_MAGIC) return false;
    if (rec->version != IDENTITY_VERSION) return false;
    if (rec->crc32 != crc32_calc((const uint8_t *)rec, IDENTITY_CRC_LEN)) return false;
    return true;
}

/* 写入一个副本：先擦扇区，再写记录 */
static HAL_StatusTypeDef copy_write(int idx, const identity_record_t *rec)
{
    HAL_StatusTypeDef st = storage_flash_erase(s_copy_addr[idx]);
    if (st != HAL_OK) return st;
    return storage_flash_write(s_copy_addr[idx], (const uint8_t *)rec,
                                    sizeof(*rec));
}

HAL_StatusTypeDef storage_identity_init(void)
{
    s_read_error = false;
    s_copy_blank[0] = s_copy_blank[1] = false;
    identity_record_t rec_a = {0}, rec_b = {0};
    bool ok_a = copy_read(0, &rec_a);
    bool ok_b = copy_read(1, &rec_b);

    if (s_read_error || (!ok_a && !ok_b && !(s_copy_blank[0] && s_copy_blank[1])))
        return HAL_ERROR;

    s_newer = -1;
    s_valid = false;
    s_generation = 0;
    memset(s_id, 0, sizeof(s_id));

    if (ok_a && ok_b) {
        s_newer = (rec_a.generation >= rec_b.generation) ? 0 : 1;
    } else if (ok_a) {
        s_newer = 0;
    } else if (ok_b) {
        s_newer = 1;
    } else {
        return HAL_OK;   /* 两份都无效：空设备，等待上层写入 */
    }

    const identity_record_t *rec = (s_newer == 0) ? &rec_a : &rec_b;
    s_valid      = true;
    s_generation = rec->generation;
    memcpy(s_id, rec->gateway_id, sizeof(s_id));
    return HAL_OK;
}

bool storage_identity_has_id(void)
{
    return s_valid;
}

HAL_StatusTypeDef storage_identity_get(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return HAL_ERROR;
    if (!s_valid) return HAL_ERROR;

    size_t n = out_size - 1;
    if (n > sizeof(s_id)) n = sizeof(s_id);
    memcpy(out, s_id, n);
    out[n] = '\0';
    return HAL_OK;
}

static HAL_StatusTypeDef storage_identity_set_impl(const char *gateway_id)
{
    if (gateway_id == NULL) return HAL_ERROR;

    identity_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic      = IDENTITY_MAGIC;
    rec.version    = IDENTITY_VERSION;
    rec.generation = s_valid ? (s_generation + 1) : 1;

    /* 长度超出则拒绝，避免静默截断产生重复 ID */
    size_t len = strlen(gateway_id);
    if (len >= GATEWAY_ID_MAX_LEN) return HAL_ERROR;
    memcpy(rec.gateway_id, gateway_id, len);
    rec.crc32 = crc32_calc((const uint8_t *)&rec, IDENTITY_CRC_LEN);

    /* 写较旧的那份：没有有效副本时写 A */
    int target = (s_newer < 0) ? 0 : (1 - s_newer);

    HAL_StatusTypeDef st = copy_write(target, &rec);
    if (st != HAL_OK) return st;

    /* 回读校验，确认真的写进去了 */
    identity_record_t verify;
    if (!copy_read(target, &verify)) return HAL_ERROR;

    s_valid      = true;
    s_generation = rec.generation;
    memcpy(s_id, rec.gateway_id, sizeof(s_id));
    s_newer      = target;
    return HAL_OK;
}

/* 保存新的设备身份并保留另一有效副本。 */
HAL_StatusTypeDef storage_identity_set(const char *gateway_id)
{
    if (!storage_mutation_begin()) return HAL_BUSY;
    HAL_StatusTypeDef st = storage_identity_set_impl(gateway_id);
    storage_mutation_end();
    return st;
}
