#include "upgrade_tf.h"

#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"
#include "mbedtls/sha256.h"

#include <string.h>

#define TF_PASS_TIMEOUT_MS 180000U
#define TF_CLOSE_TIMEOUT_MS 3000U

static UpgradeTfSource source;
static bool bound;
static bool busy;
static bool file_open;
static bool cancelled;
static UpgradeEntryPeer active_peer;
static uint32_t active_session;
static uint8_t data[UPGRADE_STAGE_MAX_CHUNK_SIZE] __attribute__((aligned(8)));

// 在临界区读取导入取消标记。
static bool Cancelled(void)
{
  taskENTER_CRITICAL();
  bool value = cancelled;
  taskEXIT_CRITICAL();
  return value;
}

// 计算本遍文件处理剩余的毫秒数。
static uint32_t Remaining(uint32_t started_ms)
{
  uint32_t elapsed = (uint32_t)(HAL_GetTick() - started_ms);
  return elapsed < TF_PASS_TIMEOUT_MS ? TF_PASS_TIMEOUT_MS - elapsed : 0U;
}

// 有界关闭文件租约，仅在明确成功后清除打开状态。
static UpgradeStageResult CloseFile(void)
{
  if (!file_open)
  {
    return UPGRADE_STAGE_OK;
  }
  uint32_t started_ms = HAL_GetTick();
  UpgradeStageResult result = source.close(source.context, TF_CLOSE_TIMEOUT_MS);
  if ((uint32_t)(HAL_GetTick() - started_ms) >= TF_CLOSE_TIMEOUT_MS)
  {
    result = UPGRADE_STAGE_TIMEOUT;
  }
  if (result == UPGRADE_STAGE_OK)
  {
    file_open = false;
  }
  return result;
}

// 校验并保存唯一 TF 文件服务绑定。
UpgradeStageResult UpgradeTf_Bind(const UpgradeTfSource *binding)
{
  if ((binding == NULL) || (binding->open == NULL) || (binding->read == NULL) || (binding->close == NULL))
  {
    return UPGRADE_STAGE_INVALID;
  }
  if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning))
  {
    return UPGRADE_STAGE_MODE;
  }
  taskENTER_CRITICAL();
  if (bound)
  {
    taskEXIT_CRITICAL();
    return UPGRADE_STAGE_BUSY;
  }
  source = *binding;
  bound = true;
  taskEXIT_CRITICAL();
  return UPGRADE_STAGE_OK;
}

// 匹配当前导入身份后，请求停止导入及已建立的收包会话。
UpgradeStageResult UpgradeTf_RequestCancel(const UpgradeEntryPeer *peer)
{
  if ((peer == NULL) || (__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning))
  {
    return UPGRADE_STAGE_MODE;
  }
  taskENTER_CRITICAL();
  bool matches = busy && (peer->source == active_peer.source) && (peer->epoch == active_peer.epoch);
  uint32_t session_id = active_session;
  if (matches)
  {
    cancelled = true;
  }
  taskEXIT_CRITICAL();
  if (matches && (session_id != 0U))
  {
    (void)UpgradeStage_RequestAbort(session_id);
  }
  return matches ? UPGRADE_STAGE_OK : UPGRADE_STAGE_MODE;
}

// 导入任务独占data；offset<size，长度单位B、期限单位ms。仅成功后可消费本块。
static UpgradeStageResult ReadBlock(uint32_t offset, uint32_t size, uint32_t started_ms,
                                     uint32_t wait_limit_ms, uint32_t *count)
{
  *count = size - offset;
  if (*count > sizeof(data))
  {
    *count = sizeof(data);
  }
  if (Cancelled())
  {
    return UPGRADE_STAGE_MODE;
  }
  uint32_t remaining = Remaining(started_ms);
  if (remaining == 0U)
  {
    return UPGRADE_STAGE_TIMEOUT;
  }
  if (remaining > wait_limit_ms)
  {
    remaining = wait_limit_ms;
  }
  UpgradeStageResult result = source.read(source.context, offset, data, *count, remaining);
  if (Remaining(started_ms) == 0U)
  {
    return UPGRADE_STAGE_TIMEOUT;
  }
  return (result == UPGRADE_STAGE_OK) && Cancelled() ? UPGRADE_STAGE_MODE : result;
}

// 第一遍计算整包传输摘要；第二遍复制后再读回核对，拒绝两遍之间发生变化的文件。
static UpgradeStageResult HashFile(uint32_t size, uint8_t *digest)
{
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  UpgradeStageResult result = mbedtls_sha256_starts(&hash, 0) == 0
                            ? UPGRADE_STAGE_OK : UPGRADE_STAGE_INTERNAL;
  uint32_t started_ms = HAL_GetTick();
  for (uint32_t offset = 0U; (result == UPGRADE_STAGE_OK) && (offset < size);)
  {
    uint32_t count;
    result = ReadBlock(offset, size, started_ms, TF_PASS_TIMEOUT_MS, &count);
    if ((result == UPGRADE_STAGE_OK) && (mbedtls_sha256_update(&hash, data, count) != 0))
    {
      result = UPGRADE_STAGE_INTERNAL;
    }
    offset += count;
  }
  if ((result == UPGRADE_STAGE_OK) && (mbedtls_sha256_finish(&hash, digest) != 0))
  {
    result = UPGRADE_STAGE_INTERNAL;
  }
  mbedtls_sha256_free(&hash);
  return result;
}

// 逐块读取固定文件并写入升级会话，沿用本遍总期限。
static UpgradeStageResult CopyFile(const UpgradeEntryPeer *peer, uint32_t session_id, uint32_t size)
{
  uint32_t started_ms = HAL_GetTick();
  for (uint32_t offset = 0U; offset < size;)
  {
    // 文件等待也不能跨过接收服务的30s无进展期限。
    uint32_t count;
    UpgradeStageResult result = ReadBlock(offset, size, started_ms, UPGRADE_STAGE_IDLE_TIMEOUT_MS, &count);
    if (result != UPGRADE_STAGE_OK)
    {
      return result;
    }
    result = UpgradeEntry_Write(peer, session_id, offset, data, count);
    if (result != UPGRADE_STAGE_OK)
    {
      return result;
    }
    offset += count;
  }
  return UPGRADE_STAGE_OK;
}

// 取得文件租约，先算摘要再复制，关闭文件后完成认证提交。
UpgradeStageResult UpgradeTf_Import(const UpgradeEntryPeer *peer, const char *path, UpgradeTfResult *output)
{
  if (output == NULL)
  {
    return UPGRADE_STAGE_INVALID;
  }
  memset(output, 0, sizeof(*output));
  if ((peer == NULL) || (peer->source != UPGRADE_STAGE_SOURCE_TF) || (peer->epoch == 0U)
      || (path == NULL) || (strcmp(path, UPGRADE_TF_PATH) != 0))
  {
    output->result = UPGRADE_STAGE_INVALID;
    return output->result;
  }
  if (!UpgradeStage_ExecutorAllowed())
  {
    output->result = UPGRADE_STAGE_MODE;
    return output->result;
  }
  UpgradeStageStatus status;
  output->result = UpgradeEntry_GetStatus(peer, &status);
  if (output->result != UPGRADE_STAGE_OK)
  {
    return output->result;
  }
  taskENTER_CRITICAL();
  bool ready = bound;
  bool already_busy = busy;
  if (ready && !already_busy)
  {
    busy = true;
    cancelled = false;
    active_peer = *peer;
    active_session = 0U;
  }
  taskEXIT_CRITICAL();
  if (!ready || already_busy)
  {
    output->result = ready ? UPGRADE_STAGE_BUSY : UPGRADE_STAGE_NOT_READY;
    return output->result;
  }
  if (!status.backend_ready || status.busy || status.cleanup_pending
      || ((status.phase != UPGRADE_STAGE_IDLE) && (status.phase != UPGRADE_STAGE_ERROR)))
  {
    output->result = status.backend_ready ? UPGRADE_STAGE_BUSY : UPGRADE_STAGE_NOT_READY;
  }
  else
  {
    output->result = CloseFile(); // 上一次关闭不明确时，先确认旧租约结束。
    uint32_t size = 0U;
    uint8_t digest[32] = {0};
    if (output->result == UPGRADE_STAGE_OK)
    {
      file_open = true; // open失败也可能取得租约，必须尝试幂等close。
      uint32_t started_ms = HAL_GetTick();
      output->result = source.open(source.context, UPGRADE_TF_PATH, &size, TF_CLOSE_TIMEOUT_MS);
      if ((uint32_t)(HAL_GetTick() - started_ms) >= TF_CLOSE_TIMEOUT_MS)
      {
        output->result = UPGRADE_STAGE_TIMEOUT;
      }
    }
    if ((output->result == UPGRADE_STAGE_OK)
        && ((size < UPGRADE_STAGE_MIN_PACKAGE_SIZE) || (size > UPGRADE_STAGE_MAX_PACKAGE_SIZE)))
    {
      output->result = UPGRADE_STAGE_INVALID;
    }
    if (output->result == UPGRADE_STAGE_OK)
    {
      output->result = HashFile(size, digest);
    }
    if ((output->result == UPGRADE_STAGE_OK) && !Cancelled())
    {
      output->result = UpgradeEntry_Begin(peer, UPGRADE_TF_PATH, size, digest, &output->session_id);
      taskENTER_CRITICAL();
      active_session = output->session_id;
      taskEXIT_CRITICAL();
    }
    else if (output->result == UPGRADE_STAGE_OK)
    {
      output->result = UPGRADE_STAGE_MODE;
    }
    if (output->result == UPGRADE_STAGE_OK)
    {
      output->result = CopyFile(peer, output->session_id, size);
    }
    output->close_error = CloseFile();
    if ((output->result == UPGRADE_STAGE_OK) && (output->close_error != UPGRADE_STAGE_OK))
    {
      output->result = output->close_error;
    }
    if (output->result == UPGRADE_STAGE_OK)
    {
      // Finish 失败时保留其阶段；VERIFIED 或提交不明时，由接收服务决定后续清理。
      output->result = UpgradeEntry_Finish(peer, output->session_id);
    }
    else if (output->session_id != 0U)
    {
      output->abort_error = UpgradeEntry_Abort(peer, output->session_id);
    }
  }
  taskENTER_CRITICAL();
  busy = false;
  taskEXIT_CRITICAL();
  return output->result;
}
