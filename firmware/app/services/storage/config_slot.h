#ifndef CONFIG_SLOT_H
#define CONFIG_SLOT_H

#include <stdint.h>

/*
 * 配置槽结构定义（依据设计说明书 6.2 节）。
 *
 * 每槽 80KiB 布局：
 *   ┌──────────────────────────────────┐ 0x00
 *   │ config_slot_header_t (64B)       │
 *   ├──────────────────────────────────┤ 0x40
 *   │ JSON 数据 (最多 64KiB)            │
 *   ├──────────────────────────────────┤ 0x10040
 *   │ 提交标记 commit_marker (4B)      │
 *   └──────────────────────────────────┘ 0x14000
 *
 * 提交标记在数据全部写入并校验后才写，用于判断槽是否完整。
 */

#define CONFIG_SLOT_MAGIC          0x43464753U  /* "CFGS" */
#define CONFIG_SLOT_MARKER_VALID   0xA55A5AA5U
#define CONFIG_SLOT_MARKER_EMPTY   0xFFFFFFFFU

typedef struct __attribute__((packed)) {
    uint32_t magic;            /* 固定 CONFIG_SLOT_MAGIC */
    uint32_t generation;       /* 代次，越大越新 */
    uint32_t json_length;      /* JSON 数据字节数 */
    uint32_t json_crc32;       /* JSON 数据 CRC32 */
    uint32_t header_crc32;     /* 头部 CRC32（覆盖 magic~json_crc32） */
    uint8_t  reserved[44];     /* 预留，对齐到 64B */
} config_slot_header_t;

_Static_assert(sizeof(config_slot_header_t) == 64, "config_slot_header_t must be 64 bytes");

#endif /* CONFIG_SLOT_H */
