#ifndef MAINT_FRAME_H
#define MAINT_FRAME_H

#include <stdint.h>

#define MAINT_FRAME_VERSION 1U
// 整帧载荷上限；Boot CHUNK 占用前 8 B 保存会话号和偏移，余下最多 1016 B 才是包数据。
#define MAINT_FRAME_PAYLOAD_MAX 1024U
#define MAINT_FRAME_HEADER_SIZE 11U
#define MAINT_FRAME_OVERHEAD 15U
#define MAINT_FRAME_RESPONSE 1U
#define MAINT_FRAME_PARTIAL_TIMEOUT_MS 2000U
#define MAINT_FRAME_SESSION_TIMEOUT_MS 30000U

typedef enum
{
    MAINT_FRAME_OK,
    MAINT_FRAME_MORE,
    MAINT_FRAME_FORMAT,
    MAINT_FRAME_VERSION_ERROR,
    MAINT_FRAME_RANGE,
    MAINT_FRAME_INTEGRITY
} maint_frame_result_t;

typedef struct
{
    uint8_t opcode;
    uint8_t flags;
    uint32_t request_id;
    uint16_t payload_len;
    const uint8_t *payload;
} maint_frame_t;

/*
 * 解码连续缓冲中的首帧。MORE时保留未消费数据；其他结果按consumed丢弃前缀后继续。
 * frame不得与输入重叠；成功payload引用输入缓冲，处理完才能移动或复用；长度单位B。
 * 不分配缓冲、不访问UART；调用方按上述期限清理半帧和未提交会话。
 * 帧 CRC 只检查传输完整性，命令权限和固件包认证由各自使用方处理。
 */
maint_frame_result_t maint_frame_decode(const uint8_t *data, uint16_t size,
                                        maint_frame_t *frame, uint16_t *consumed);

/* 编码一帧；frame及payload均不与out重叠，容量/长度单位B，失败清零written。 */
maint_frame_result_t maint_frame_encode(const maint_frame_t *frame, uint8_t *out,
                                        uint16_t capacity, uint16_t *written);

#endif
