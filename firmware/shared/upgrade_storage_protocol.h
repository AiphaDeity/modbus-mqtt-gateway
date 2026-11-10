#ifndef UPGRADE_STORAGE_PROTOCOL_H
#define UPGRADE_STORAGE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define UPGRADE_STORAGE_PROTOCOL_VERSION 2U
#define UPGRADE_STORAGE_CHUNK_SIZE 256U

typedef enum
{
  UPGRADE_STORAGE_BEGIN = 0,
  UPGRADE_STORAGE_CHUNK,
  UPGRADE_STORAGE_FINISH,
  UPGRADE_STORAGE_ABORT,
  UPGRADE_STORAGE_READ_PACKAGE,
  UPGRADE_STORAGE_READ_METADATA,
  UPGRADE_STORAGE_RESERVE_TX,
  UPGRADE_STORAGE_COMMIT_READY
} UpgradeStorageRequestType;

typedef enum
{
  UPGRADE_STORAGE_OK = 0,
  UPGRADE_STORAGE_INVALID,
  UPGRADE_STORAGE_BUSY,
  UPGRADE_STORAGE_IO_ERROR,
  UPGRADE_STORAGE_TIMEOUT,
  UPGRADE_STORAGE_CORRUPT,
  UPGRADE_STORAGE_UNSUPPORTED,
  UPGRADE_STORAGE_CONFLICT,
  UPGRADE_STORAGE_NOT_READY
} UpgradeStorageResult;

// 原生值拷贝消息，非网络/Flash编码；槽0/A、1/B，未用字段清零，不携带指针。
typedef struct
{
  uint32_t req_id;
  uint32_t session;
  uint32_t offset;
  uint32_t pkg_len;
  uint64_t tx_id;
  uint32_t expected_normal_sequence;
  uint32_t expected_recovery_sequence;
  uint32_t expected_normal_result;
  uint32_t expected_recovery_result;
  uint16_t len;
  uint8_t type;
  uint8_t slot;
  uint8_t data[UPGRADE_STORAGE_CHUNK_SIZE];
  uint32_t deadline_ms; // HAL 毫秒绝对期限；存储端在开始及分块修改前检查，不因出队续期。
} UpgradeStorageRequest;

typedef struct
{
  uint32_t req_id;
  uint32_t session;
  int32_t result;
  uint32_t written;
  uint64_t tx_id;
  uint16_t actual_len;
  uint8_t session_ended;
  uint8_t type;
  uint32_t detail;
  uint8_t data[UPGRADE_STORAGE_CHUNK_SIZE];
} UpgradeStorageResponse;

_Static_assert(sizeof(UpgradeStorageRequest) == 304U, "Storage request ABI");
_Static_assert(offsetof(UpgradeStorageRequest, data) == 44U, "Storage request data offset");
_Static_assert(offsetof(UpgradeStorageRequest, deadline_ms) == 300U, "Storage deadline offset");
_Static_assert(sizeof(UpgradeStorageResponse) == 288U, "Storage response ABI");
_Static_assert(offsetof(UpgradeStorageResponse, data) == 32U, "Storage response data offset");

#endif
