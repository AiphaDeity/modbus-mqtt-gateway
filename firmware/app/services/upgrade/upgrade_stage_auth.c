#include "upgrade_stage_auth.h"

#include "upgrade_package.h"
#include "app_monitor.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os2.h"

#include <string.h>

typedef struct
{
  const UpgradeStageBackend *backend;
  void *context;
  uint32_t session_id;
  uint32_t total_size;
  uint32_t started_ms;
  UpgradeStageResult io_result;
} AuthContext;

// 从包头字节缓冲读取小端密钥编号。
static uint32_t Read32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8U)
       | ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

// 向公共认证层提供 HAL 单调毫秒时基。
static uint32_t NowMs(void *context)
{
  (void)context;
  return HAL_GetTick();
}

// 检查取消和原期限，有实际进展时让出 CPU；喂狗由监督任务决定。
static bool Checkpoint(void *context, uint32_t started_ms, uint32_t timeout_ms, bool progress)
{
  AuthContext *auth = context;
  if ((auth->session_id != 0U) && UpgradeStage_CancellationRequested(auth->session_id))
  {
    auth->io_result = UPGRADE_STAGE_MODE;
    return false;
  }
  if ((uint32_t)(HAL_GetTick() - started_ms) >= timeout_ms)
  {
    return false;
  }
  if (progress && (!Monitor_UpgradeProgress() || (osThreadYield() != osOK)))
  {
    auth->io_result = UPGRADE_STAGE_INTERNAL;
    return false;
  }
  return (uint32_t)(HAL_GetTick() - started_ms) < timeout_ms;
}

// 在认证剩余期限内读取指定包区间，保留存储错误。
static bool ReadPackage(void *context, size_t offset, uint8_t *data, size_t size)
{
  AuthContext *auth = context;
  uint32_t elapsed = (uint32_t)(HAL_GetTick() - auth->started_ms);
  if (elapsed >= UPGRADE_STAGE_VERIFY_TIMEOUT_MS)
  {
    auth->io_result = UPGRADE_STAGE_TIMEOUT;
    return false;
  }
  if ((offset > auth->total_size) || (size > auth->total_size - offset))
  {
    auth->io_result = UPGRADE_STAGE_INVALID;
    return false;
  }
  auth->io_result = auth->backend->read(auth->context, auth->session_id, (uint32_t)offset,
                                       data, (uint32_t)size,
                                       UPGRADE_STAGE_VERIFY_TIMEOUT_MS - elapsed);
  if ((uint32_t)(HAL_GetTick() - auth->started_ms) >= UPGRADE_STAGE_VERIFY_TIMEOUT_MS)
  {
    auth->io_result = UPGRADE_STAGE_TIMEOUT;
  }
  return auth->io_result == UPGRADE_STAGE_OK;
}

// 复用分段P-256验签和监督回调；本函数须与包认证串行，不修改持久状态。
UpgradeStageResult UpgradeStage_AuthenticateNotice(const UpgradeStageBackend *adapter, void *context,
                                                  const uint8_t *digest, const uint8_t *signature)
{
  const uint8_t *public_key = NULL;
  const uint8_t *aes_key = NULL;
  UpgradeStageResult result = adapter->keys(context, 1U, 1U, &public_key, &aes_key);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (public_key == NULL || public_key[0] != 0x04U)
  {
    return UPGRADE_STAGE_NOT_READY;
  }
  uint32_t started_ms = HAL_GetTick();
  AuthContext auth = {0};
  UpgradePackagePlatform platform = {&auth, NowMs, Checkpoint, NULL};
  UpgradePackageOperation operation = {&platform, started_ms,
    UPGRADE_PACKAGE_SIGNATURE_TIMEOUT_MS, UPGRADE_PACKAGE_ERROR_NONE};
  if (UpgradePackage_VerifyDigestResult(&operation, digest, signature, public_key) == 0)
  {
    return UPGRADE_STAGE_OK;
  }
  if (auth.io_result != UPGRADE_STAGE_OK) return auth.io_result;
  if (operation.error == UPGRADE_PACKAGE_ERROR_TIMEOUT) return UPGRADE_STAGE_TIMEOUT;
  return operation.error == UPGRADE_PACKAGE_ERROR_INTERNAL ? UPGRADE_STAGE_INTERNAL : UPGRADE_STAGE_AUTH;
}

// 解析密钥编号并调用公共认证，映射首个失败原因。
UpgradeStageResult UpgradeStage_Authenticate(const UpgradeStageBackend *backend, void *context,
                                            uint32_t session_id, uint32_t total_size,
                                            uint32_t finish_started_ms, UpgradePackageImage *image)
{
  if (image == NULL)
  {
    return UPGRADE_STAGE_INVALID;
  }
  memset(image, 0, sizeof(*image));
  if ((backend == NULL) || (backend->read == NULL) || (backend->keys == NULL))
  {
    return UPGRADE_STAGE_NOT_READY;
  }
  AuthContext auth = {backend, context, session_id, total_size, finish_started_ms, UPGRADE_STAGE_OK};
  UpgradePackagePlatform platform = {&auth, NowMs, Checkpoint, NULL};
  UpgradePackageOperation operation = {&platform, finish_started_ms,
                                       UPGRADE_STAGE_VERIFY_TIMEOUT_MS, UPGRADE_PACKAGE_ERROR_NONE};
  uint8_t manifest[UPGRADE_PACKAGE_MANIFEST_SIZE];
  uint32_t firmware_size = 0U;
  if (!ReadPackage(&auth, 0U, manifest, sizeof(manifest)))
  {
    return auth.io_result;
  }
  if (!UpgradePackage_CheckManifest(manifest, sizeof(manifest), total_size, &firmware_size, NULL))
  {
    return UPGRADE_STAGE_INVALID;
  }
  if (!Checkpoint(&auth, finish_started_ms, UPGRADE_STAGE_VERIFY_TIMEOUT_MS, false))
  {
    return UPGRADE_STAGE_TIMEOUT;
  }
  const uint8_t *public_key = NULL;
  const uint8_t *aes_key = NULL;
  UpgradeStageResult result = backend->keys(context,
                                            Read32(manifest + UPGRADE_PACKAGE_SIGNING_KEY_ID_OFFSET),
                                            Read32(manifest + UPGRADE_PACKAGE_DECRYPT_KEY_ID_OFFSET),
                                            &public_key, &aes_key);
  if (!Checkpoint(&auth, finish_started_ms, UPGRADE_STAGE_VERIFY_TIMEOUT_MS, false))
  {
    return UPGRADE_STAGE_TIMEOUT;
  }
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if ((public_key == NULL) || (aes_key == NULL) || (public_key[0] != 0x04U))
  {
    return UPGRADE_STAGE_NOT_READY;
  }
  if (UpgradePackage_VerifyImage(&operation, ReadPackage, &auth, total_size,
                                  public_key, aes_key, image))
  {
    return UPGRADE_STAGE_OK;
  }
  if (auth.io_result != UPGRADE_STAGE_OK)
  {
    return auth.io_result;
  }
  switch (operation.error)
  {
    case UPGRADE_PACKAGE_ERROR_TIMEOUT:
      return UPGRADE_STAGE_TIMEOUT;
    case UPGRADE_PACKAGE_ERROR_IO:
      return UPGRADE_STAGE_STORAGE;
    case UPGRADE_PACKAGE_ERROR_INTERNAL:
      return UPGRADE_STAGE_INTERNAL;
    case UPGRADE_PACKAGE_ERROR_INTEGRITY:
      return UPGRADE_STAGE_INTEGRITY;
    case UPGRADE_PACKAGE_ERROR_INVALID:
      return UPGRADE_STAGE_INVALID;
    default:
      return UPGRADE_STAGE_AUTH;
  }
}
