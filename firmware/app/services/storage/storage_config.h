#ifndef STORAGE_CONFIG_H
#define STORAGE_CONFIG_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * 配置存储服务 —— A/B/C 三槽事务（依据设计说明书 6.2 节）。
 *
 * 三槽：
 *   A = 0x000000, B = 0x014000, C = 0x028000（每槽 80KiB）
 *   R = 启动时选出的完整槽，本次启动固定
 *   S = 最近成功保存槽
 *   C = 候选槽（既非 R 也非 S）
 *
 * 事务流程：
 *   BEGIN（擦候选）→ CHUNK（分块写 JSON）→ VALIDATE（校验 CRC）
 *   → COMMIT（写提交标记，候选变新 S）
 *
 * M3 只负责持久化 JSON 字节流，不解析 JSON 内容。
 * JSON 解析与字段校验由配置维护模块负责。
 */

/*
 * 以下直接介质接口仅在Storage_Task调用；其他任务使用storage_runtime_config传值。
 * 配置读取句柄（流式读取，不占用大块 RAM）。
 * M3 只按块把 JSON 字节流交给调用方，解析由配置维护模块负责。
 */
typedef struct {
    uint32_t slot_idx;    /* 当前运行槽索引（内部使用） */
    uint32_t generation;  /* 配置代次 */
    uint32_t total_len;   /* JSON 总字节数 */
    uint32_t crc32;       /* JSON 的 CRC32，供调用方整体校验 */
} storage_config_reader_t;

// 打开当前有效配置：填充句柄信息（不复制数据）
HAL_StatusTypeDef storage_config_open(storage_config_reader_t *rd);

// 从当前配置中按块读取（offset 从 0 开始，len 由调用方决定）
HAL_StatusTypeDef storage_config_read_chunk(const storage_config_reader_t *rd,
                                             uint32_t offset, uint8_t *buf,
                                             uint32_t len);

// 开始配置写入事务；失败也可能已取得session_id，须由原调用方取消。
HAL_StatusTypeDef storage_config_begin(uint32_t total_len, uint32_t *session_id);

// 分块写入1～256 B，offset以B计且从0连续递增；同步返回前不保留data。
HAL_StatusTypeDef storage_config_chunk(uint32_t session_id, uint32_t offset,
                                        const uint8_t *data, uint32_t len);

// 读回全部数据，与CHUNK接收字节的CRC32比较；通过后才能提交。
HAL_StatusTypeDef storage_config_validate(uint32_t session_id);

// 提交后候选成为S，重启才重新选R；应答不明须只读RESOLVE，不重复擦写。
HAL_StatusTypeDef storage_config_commit(uint32_t session_id);

// 取消当前事务（不影响 R 和旧 S）
void storage_config_abort(uint32_t session_id);

// 获取当前运行配置的 generation
uint32_t storage_config_get_generation(void);

// 是否存在有效配置
bool storage_config_has_valid(void);
/* 只读恢复：全空视为未配置，读取失败或已提交记录损坏返回错误。 */
HAL_StatusTypeDef storage_config_check(void);
/* Storage_Task结算时检查RAM配置事务，不代替调用方取消。 */
bool storage_config_tx_active(void);

/* Config客户端通过原Storage运行队列传值；数据只在同步调用期间访问。 */
#define STORAGE_CONFIG_CHUNK_MAX 256U
typedef enum {
    STORAGE_CONFIG_READ_RUNNING = 1,
    STORAGE_CONFIG_READ_SAVED,
    STORAGE_CONFIG_BEGIN,
    STORAGE_CONFIG_CHUNK,
    STORAGE_CONFIG_VALIDATE,
    STORAGE_CONFIG_COMMIT,
    STORAGE_CONFIG_RESOLVE,
    STORAGE_CONFIG_ABORT,
    STORAGE_CONFIG_TF_PROBE,
    STORAGE_CONFIG_TF_OPEN,
    STORAGE_CONFIG_TF_READ,
    STORAGE_CONFIG_TF_CLOSE
} storage_config_action_t;

typedef struct {
    storage_config_action_t action;
    uint32_t session_id;
    uint32_t offset;
    uint32_t total_len;
    uint32_t generation;
    uint32_t crc32;
    uint32_t len;
    uint8_t data[STORAGE_CONFIG_CHUNK_MAX];
} storage_config_message_t;

/*
 * READ无配置时total_len=0，RESOLVE只读核对提交。
 * TF操作走同一值拷贝RPC：PROBE的total_len为固定文件位图；OPEN返回文件长度，
 * session_id为原租约号，READ完整填充len，CLOSE明确成功前不能丢弃租约。
 */
HAL_StatusTypeDef storage_config_process(storage_config_message_t *message);

#endif /* STORAGE_CONFIG_H */
