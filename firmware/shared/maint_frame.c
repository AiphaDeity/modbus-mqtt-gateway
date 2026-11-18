/**
 * @file maint_frame.c
 * @brief App配置与Boot恢复共用的维护帧V1编解码，不处理命令权限。
 */
#include "maint_frame.h"
#include <stddef.h>
#include <string.h>

/** @brief 读取帧中的小端16位字段。 */
static uint16_t maint_read16(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8U);
}

/** @brief 读取帧中的小端32位字段。 */
static uint32_t maint_read32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

/** @brief 写入小端32位字段；载荷长度由调用方单独写两个字节。 */
static void maint_write32(uint8_t *data, uint32_t value)
{
    for (uint32_t i = 0U; i < 4U; i++) data[i] = (uint8_t)(value >> (8U * i));
}

/** @brief CRC-32/ISO-HDLC只覆盖version到payload，不包含magic和CRC字段。 */
static uint32_t maint_crc32(const uint8_t *data, uint16_t size)
{
    uint32_t crc = UINT32_MAX;
    for (uint16_t i = 0U; i < size; i++)
    {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++)
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
    }
    return ~crc;
}

/*
 * 从连续输入中解码首帧，只验证维护帧结构与 CRC，不执行命令或判定升级权限。
 * MORE 时保留半帧；错误时报告可丢弃前缀，成功 payload 借用输入缓冲，须同步处理后再复用。
 */
maint_frame_result_t maint_frame_decode(const uint8_t *data, uint16_t size,
                                        maint_frame_t *frame, uint16_t *consumed)
{
    if (consumed == NULL || frame == NULL) return MAINT_FRAME_FORMAT;
    *consumed = 0U;
    memset(frame, 0, sizeof(*frame));
    if (data == NULL) return MAINT_FRAME_FORMAT;
    if (size == 0U) return MAINT_FRAME_MORE;
    // 1. 寻找 A5 5A 帧头，丢掉明确无关的前缀，保留可能尚未收齐的下一帧。
    uint16_t prefix = 0U;
    while (prefix < size && (data[prefix] != 0xA5U ||
           (prefix + 1U < size && data[prefix + 1U] != 0x5AU))) prefix++;
    if (prefix != 0U)
    {
        *consumed = prefix;
        return MAINT_FRAME_FORMAT;
    }
    // 2. 核对版本、标志和声明长度；数据未收齐时不消费候选帧。
    if (size < 3U) return MAINT_FRAME_MORE;
    *consumed = 1U;
    if (data[2] != MAINT_FRAME_VERSION) return MAINT_FRAME_VERSION_ERROR;
    if (size < MAINT_FRAME_HEADER_SIZE)
    {
        *consumed = 0U;
        return MAINT_FRAME_MORE;
    }
    if ((data[4] & (uint8_t)~MAINT_FRAME_RESPONSE) != 0U) return MAINT_FRAME_FORMAT;
    uint16_t payload_len = maint_read16(data + 9U);
    if (payload_len > MAINT_FRAME_PAYLOAD_MAX) return MAINT_FRAME_RANGE;
    uint16_t total = (uint16_t)(MAINT_FRAME_OVERHEAD + payload_len);
    if (size < total)
    {
        *consumed = 0U;
        return MAINT_FRAME_MORE;
    }
    // 3. 完整帧到齐才比较传输 CRC，不能用它替代恢复包的签名/GCM 认证。
    if (maint_read32(data + MAINT_FRAME_HEADER_SIZE + payload_len) !=
        maint_crc32(data + 2U, (uint16_t)(9U + payload_len))) return MAINT_FRAME_INTEGRITY;
    // 4. 输出操作码、请求号及载荷视图，具体 INFO/BEGIN/CHUNK/FINISH 由 Boot 的 Handle 分发。
    frame->opcode = data[3];
    frame->flags = data[4];
    frame->request_id = maint_read32(data + 5U);
    frame->payload_len = payload_len;
    frame->payload = data + MAINT_FRAME_HEADER_SIZE;
    *consumed = total;
    return MAINT_FRAME_OK;
}

/** @brief 将公开字段编码为维护帧，不保留任何调用者缓冲。 */
maint_frame_result_t maint_frame_encode(const maint_frame_t *frame, uint8_t *out,
                                        uint16_t capacity, uint16_t *written)
{
    if (written == NULL) return MAINT_FRAME_FORMAT;
    *written = 0U;
    if (frame == NULL || out == NULL ||
        (frame->flags & (uint8_t)~MAINT_FRAME_RESPONSE) != 0U ||
        (frame->payload_len != 0U && frame->payload == NULL)) return MAINT_FRAME_FORMAT;
    if (frame->payload_len > MAINT_FRAME_PAYLOAD_MAX ||
        capacity < MAINT_FRAME_OVERHEAD + frame->payload_len) return MAINT_FRAME_RANGE;
    out[0] = 0xA5U;
    out[1] = 0x5AU;
    out[2] = MAINT_FRAME_VERSION;
    out[3] = frame->opcode;
    out[4] = frame->flags;
    maint_write32(out + 5U, frame->request_id);
    out[9] = (uint8_t)frame->payload_len;
    out[10] = (uint8_t)(frame->payload_len >> 8U);
    if (frame->payload_len != 0U)
        memcpy(out + MAINT_FRAME_HEADER_SIZE, frame->payload, frame->payload_len);
    maint_write32(out + MAINT_FRAME_HEADER_SIZE + frame->payload_len,
                   maint_crc32(out + 2U, (uint16_t)(9U + frame->payload_len)));
    *written = (uint16_t)(MAINT_FRAME_OVERHEAD + frame->payload_len);
    return MAINT_FRAME_OK;
}
