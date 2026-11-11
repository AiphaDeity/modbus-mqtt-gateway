#include "upgrade_tf.h"
#include "upgrade_quiesce.h"
#include "app_upgrade.h"
#include "app_monitor.h"
#include "main.h"

#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"
#include "mbedtls/sha256.h"

#include <string.h>
#include <stdio.h>

#define TF_PASS_TIMEOUT_MS 180000U
#define TF_CLOSE_TIMEOUT_MS 3000U
#define TF_KEY_DEBOUNCE_MS 40U
#define TF_KEY_HOLD_MS 3000U

typedef struct
{
  bool sample;
  bool down;
  bool armed;
  uint32_t changed_ms;
  uint32_t pressed_ms;
} TfButton;

static UpgradeTfSource source;
static bool bound;
static bool busy;
static bool file_open;
static bool cancelled;
static UpgradeEntryPeer active_peer;
static uint32_t active_session;
static uint8_t data[UPGRADE_STAGE_MAX_CHUNK_SIZE] __attribute__((aligned(8)));
static bool initialized;
static bool button_authorized;
static uint32_t button_epoch;

// 上电或导入结束后先等待一次稳定释放，忽略这之前已经按住的按键。
static void ResetButton(TfButton *button, bool pressed, uint32_t now_ms)
{
  *button = (TfButton){.sample = pressed, .down = true, .changed_ms = now_ms};
}

// 消抖确认按下后计时；仅稳定松开且已满3s时返回一次true。
static bool ButtonReleased(TfButton *button, bool pressed, uint32_t now_ms)
{
  if (pressed != button->sample)
  {
    button->sample = pressed;
    button->changed_ms = now_ms;
  }
  if ((pressed == button->down) || ((uint32_t)(now_ms - button->changed_ms) < TF_KEY_DEBOUNCE_MS))
  {
    return false;
  }
  button->down = pressed;
  if (pressed)
  {
    button->pressed_ms = now_ms;
    return false;
  }
  bool released = button->armed
               && ((uint32_t)(button->changed_ms - button->pressed_ms) >= TF_KEY_HOLD_MS);
  button->armed = true;
  return released;
}

// 只授权升级任务由本次实体按键触发的TF操作，网络来源保持未开放。
static bool AuthorizeButton(void *context, const UpgradeEntryPeer *peer)
{
  (void)context;
  if ((peer == NULL) || (peer->source != UPGRADE_STAGE_SOURCE_TF)
      || !App_IsCurrentModuleTask(APP_MODULE_UPGRADE))
  {
    return false;
  }
  taskENTER_CRITICAL();
  bool allowed = button_authorized && (peer->epoch != 0U) && (peer->epoch == button_epoch);
  taskEXIT_CRITICAL();
  return allowed;
}

// 在统一模块装配中绑定实体按键授权，不创建任务或访问介质。
AppModuleInitResult UpgradeTf_Init(void)
{
  if (!initialized)
  {
    const UpgradeEntryPolicy policy = {AuthorizeButton, NULL};
    if (UpgradeEntry_BindPolicy(&policy) != UPGRADE_STAGE_OK)
    {
      return APP_MODULE_INIT_FAILED;
    }
    initialized = true;
  }
  return APP_MODULE_INIT_OK;
}

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
  if (!Monitor_UpgradeWait(TF_CLOSE_TIMEOUT_MS))
  {
    return UPGRADE_STAGE_INTERNAL;
  }
  UpgradeStageResult result = source.close(source.context, TF_CLOSE_TIMEOUT_MS);
  if (!Monitor_UpgradeProgress())
  {
    return UPGRADE_STAGE_INTERNAL;
  }
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
  if (!Monitor_UpgradeWait(remaining))
  {
    return UPGRADE_STAGE_INTERNAL;
  }
  UpgradeStageResult result = source.read(source.context, offset, data, *count, remaining);
  if (!Monitor_UpgradeProgress())
  {
    return UPGRADE_STAGE_INTERNAL;
  }
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
      if (!Monitor_UpgradeWait(TF_CLOSE_TIMEOUT_MS))
      {
        output->result = UPGRADE_STAGE_INTERNAL;
      }
      else
      {
        output->result = source.open(source.context, UPGRADE_TF_PATH, &size, TF_CLOSE_TIMEOUT_MS);
        if (!Monitor_UpgradeProgress())
        {
          output->result = UPGRADE_STAGE_INTERNAL;
        }
      }
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

// 重试上次导入的关闭与取消；提交不明或已提交状态仍由Stage保护。
static UpgradeStageResult CleanupButtonImport(UpgradeTfResult *output)
{
  UpgradeStageResult result = CloseFile();
  if (result != UPGRADE_STAGE_OK)
  {
    output->close_error = result;
    return result;
  }
  if (button_epoch == 0U)
  {
    return UPGRADE_STAGE_OK;
  }
  const UpgradeEntryPeer peer = {UPGRADE_STAGE_SOURCE_TF, button_epoch};
  result = UpgradeEntry_Disconnect(&peer);
  if ((result != UPGRADE_STAGE_OK) && (result != UPGRADE_STAGE_MODE))
  {
    output->abort_error = result;
    return result;
  }
  return UPGRADE_STAGE_OK;
}

// 检查正常启动身份和必需依赖，通过后才按本次按键身份导入固定文件。
static void ImportFromButton(void)
{
  UpgradeStageStatus status;
  UpgradeMessage identity;
  if (!App_UpgradeCanCommit() || !App_UpgradeGetBootIdentity(&identity))
  {
    printf("TF upgrade rejected: normal Boot identity required\r\n");
    return;
  }
  if (UpgradeStage_GetStatus(&status) != UPGRADE_STAGE_OK || status.busy
      || (status.phase == UPGRADE_STAGE_QUIESCING) || (status.phase == UPGRADE_STAGE_REBOOT_PENDING))
  {
    printf("TF upgrade rejected: upgrade busy or unavailable\r\n");
    return;
  }
  if (file_open || status.cleanup_pending || (status.resume_error != UPGRADE_STAGE_OK))
  {
    UpgradeTfResult previous = {0};
    UpgradeStageResult cleanup = CleanupButtonImport(&previous);
    if (cleanup == UPGRADE_STAGE_OK)
    {
      cleanup = UpgradeStage_GetStatus(&status);
    }
    if (cleanup != UPGRADE_STAGE_OK)
    {
      printf("TF upgrade cleanup failed: result=%u close=%u abort=%u\r\n",
             (unsigned)cleanup, (unsigned)previous.close_error, (unsigned)previous.abort_error);
      return;
    }
  }
  taskENTER_CRITICAL();
  bool file_service_ready = bound;
  taskEXIT_CRITICAL();
  if (!status.backend_ready || !file_service_ready || !UpgradeQuiesce_IsReady())
  {
    printf("TF upgrade rejected: storage, trust or module callbacks not ready\r\n");
    return;
  }
  if (status.cleanup_pending || (status.resume_error != UPGRADE_STAGE_OK)
      || ((status.phase != UPGRADE_STAGE_IDLE) && (status.phase != UPGRADE_STAGE_ERROR))
      || (button_epoch == UINT32_MAX))
  {
    printf("TF upgrade rejected: previous session not settled\r\n");
    return;
  }
  if (HAL_GPIO_ReadPin(SD_CARD_DET_GPIO_Port, SD_CARD_DET_Pin) != GPIO_PIN_RESET)
  {
    printf("TF upgrade rejected: no card\r\n");
    return;
  }
  taskENTER_CRITICAL();
  button_epoch++;
  button_authorized = true;
  taskEXIT_CRITICAL();
  const UpgradeEntryPeer peer = {UPGRADE_STAGE_SOURCE_TF, button_epoch};
  UpgradeTfResult output;
  printf("TF upgrade begin: epoch=%lu\r\n", (unsigned long)peer.epoch);
  (void)UpgradeTf_Import(&peer, UPGRADE_TF_PATH, &output);
  if (output.result != UPGRADE_STAGE_OK)
  {
    (void)CleanupButtonImport(&output);
  }
  taskENTER_CRITICAL();
  button_authorized = false;
  taskEXIT_CRITICAL();
  const char *phase = UpgradeStage_GetStatus(&status) == UPGRADE_STAGE_OK
                    ? UpgradeEntry_PhaseName(status.phase) : "UNKNOWN";
  printf("TF upgrade: result=%u session=%lu phase=%s close=%u abort=%u\r\n",
         (unsigned)output.result, (unsigned long)output.session_id, phase,
         (unsigned)output.close_error, (unsigned)output.abort_error);
}

// 每20ms检测PA0；导入期间不接收按键请求，结束后必须先释放再重新长按。
void UpgradeTf_Task(void *argument)
{
  (void)argument;
  if (!initialized || !Monitor_RegisterHealth(APP_MODULE_UPGRADE, 3000U,
                                               UPGRADE_STAGE_PREPARE_TIMEOUT_MS + 1000U))
  {
    Error_Handler();
    return;
  }
  TfButton button;
  ResetButton(&button, HAL_GPIO_ReadPin(TF_UPGRADE_KEY_GPIO_Port, TF_UPGRADE_KEY_Pin) == GPIO_PIN_SET,
              HAL_GetTick());
  uint32_t poll_ticks = (osKernelGetTickFreq() + 49U) / 50U;
  for (;;)
  {
    if (!Monitor_UpgradeProgress())
    {
      Error_Handler();
      return;
    }
    bool pressed = HAL_GPIO_ReadPin(TF_UPGRADE_KEY_GPIO_Port, TF_UPGRADE_KEY_Pin) == GPIO_PIN_SET;
    if (ButtonReleased(&button, pressed, HAL_GetTick()))
    {
      ImportFromButton();
      ResetButton(&button, HAL_GPIO_ReadPin(TF_UPGRADE_KEY_GPIO_Port, TF_UPGRADE_KEY_Pin) == GPIO_PIN_SET,
                  HAL_GetTick());
    }
    if (osDelay(poll_ticks) != osOK)
    {
      Error_Handler();
      return;
    }
  }
}
