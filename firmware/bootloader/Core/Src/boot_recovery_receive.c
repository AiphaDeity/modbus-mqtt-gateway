#include "boot_recovery.h"
#include "boot_watchdog.h"
#include "flash_partition.h"
#include "maint_frame.h"
#include "upgrade_transaction.h"
#include "usart.h"

#include <stdio.h>
#include <string.h>

#define RECOVERY_ERASE_TIMEOUT_MS 300000U
#define RECOVERY_RECEIVE_TIMEOUT_MS 180000U
#define RECOVERY_REQUEST_WINDOW_MS 10000U

static uint8_t receive_buffer[MAINT_FRAME_OVERHEAD + MAINT_FRAME_PAYLOAD_MAX];
static uint16_t receive_size;
static uint32_t partial_started_ms;
static maint_frame_t last_request;

static struct
{
  bool active;
  uint32_t id;
  uint32_t begin_request_id;
  uint32_t slot;
  uint32_t total;
  uint32_t offset;
  uint32_t started_ms;
  uint32_t progress_ms;
  bool allow_unknown;
} session;

// 仅读取恢复命令使用的平面JSON字段，不分配DOM或保留输入指针。
typedef struct
{
  uint32_t slot;
  uint32_t total_len;
  uint32_t allow_unknown;
  uint32_t session_id;
  uint32_t initialize;
  uint32_t present;
} RecoveryArguments;

enum
{
  ARG_SLOT = 1U,
  ARG_TOTAL = 2U,
  ARG_ALLOW_UNKNOWN = 4U,
  ARG_SESSION = 8U,
  ARG_INITIALIZE = 16U
};

// JSON只接受协议中的ASCII空白。
static void SkipSpace(const uint8_t **cursor, const uint8_t *end)
{
  while (*cursor < end && (**cursor == ' ' || **cursor == '\t'
         || **cursor == '\r' || **cursor == '\n')) (*cursor)++;
}

// 拒绝重复/未知字段、溢出、尾随数据及非整数；布尔字段只接受true/false。
static bool ParseArguments(const maint_frame_t *frame, RecoveryArguments *args)
{
  static const char *const names[] =
    {"slot", "total_len", "allow_unknown_state", "session_id", "initialize_transactions"};
  uint32_t *values[] =
    {&args->slot, &args->total_len, &args->allow_unknown, &args->session_id, &args->initialize};
  memset(args, 0, sizeof(*args));
  const uint8_t *p = frame->payload;
  const uint8_t *end = p + frame->payload_len;
  SkipSpace(&p, end);
  if (p == end || *p++ != '{') return false;
  SkipSpace(&p, end);
  if (p < end && *p != '}')
  {
    for (;;)
    {
      if (p == end || *p++ != '"') return false;
      const uint8_t *key = p;
      while (p < end && *p != '"') p++;
      if (p == end) return false;
      uint32_t index = 0U;
      while (index < 5U && (strlen(names[index]) != (size_t)(p - key)
             || memcmp(key, names[index], (size_t)(p - key)) != 0)) index++;
      if (index == 5U || (args->present & (1U << index)) != 0U) return false;
      p++;
      SkipSpace(&p, end);
      if (p == end || *p++ != ':') return false;
      SkipSpace(&p, end);
      uint32_t value = 0U;
      if (index == 2U || index == 4U)
      {
        if (end - p >= 4 && memcmp(p, "true", 4U) == 0) { value = 1U; p += 4U; }
        else if (end - p >= 5 && memcmp(p, "false", 5U) == 0) p += 5U;
        else return false;
      }
      else
      {
        if (p == end || *p < '0' || *p > '9') return false;
        bool zero = *p == '0';
        do
        {
          uint32_t digit = (uint32_t)(*p++ - '0');
          if (value > (UINT32_MAX - digit) / 10U) return false;
          value = value * 10U + digit;
          if (p == end || *p < '0' || *p > '9') break;
          if (zero) return false;
        } while (true);
      }
      *values[index] = value;
      args->present |= 1U << index;
      SkipSpace(&p, end);
      if (p == end) return false;
      if (*p != ',') break;
      p++;
      SkipSpace(&p, end);
    }
  }
  if (p == end || *p++ != '}') return false;
  SkipSpace(&p, end);
  return p == end;
}

// 通过USART2维护口发送响应，USART1仅输出日志。
static bool Reply(const maint_frame_t *request, const char *status,
                  const char *error, const char *fields)
{
  char payload[512];
  uint8_t wire[512U + MAINT_FRAME_OVERHEAD];
  int length = snprintf(payload, sizeof(payload),
      "{\"request_id\":%lu,\"status\":\"%s\",\"error\":%s%s%s,"
      "\"field\":\"\",\"restart_required\":false%s}",
      (unsigned long)request->request_id, status,
      error == NULL ? "" : "\"", error == NULL ? "null" : error,
      error == NULL ? "" : "\"", fields);
  if (length < 0 || (size_t)length >= sizeof(payload)) return false;
  maint_frame_t response = {request->opcode, MAINT_FRAME_RESPONSE, request->request_id,
                           (uint16_t)length, (const uint8_t *)payload};
  uint16_t written = 0U;
  return maint_frame_encode(&response, wire, sizeof(wire), &written) == MAINT_FRAME_OK
      && HAL_UART_Transmit(&huart2, wire, written, 100U) == HAL_OK;
}

// 业务失败只返回公开错误；不把读取失败或未知版本当作空白。
static BootReceiveResult Reject(const maint_frame_t *request, const char *error)
{
  (void)Reply(request, "ERROR", error, "");
  return BOOT_RECEIVE_COMMAND;
}

// 保留底层状态区别，便于工具判断是否可继续操作。
static const char *StorageError(Boot_UpgradeResult result)
{
  switch (result)
  {
    case BOOT_UPGRADE_UNSUPPORTED: return "E_VERSION";
    case BOOT_UPGRADE_IO_ERROR: return "E_STORAGE";
    case BOOT_UPGRADE_CORRUPT: return "E_INTEGRITY";
    default: return "E_RANGE";
  }
}

// INFO/STATUS只读查询，未配置密钥时也可诊断，不能据此绕过认证。
static void ReplyInfo(const maint_frame_t *request)
{
  Boot_UpgradeState normal;
  Boot_RecoveryState recovery;
  UpgradeTransactionState transaction;
  Boot_UpgradeResult normal_result = Boot_UpgradeLoad(&normal);
  Boot_UpgradeResult recovery_result = Boot_RecoveryLoad(&recovery);
  Boot_UpgradeResult tx_result = Boot_UpgradeReadTransaction(&transaction);
  uint32_t protected_slot = normal_result == BOOT_UPGRADE_OK ? normal.confirmed.slot : 0U;
  if (protected_slot == 0U && recovery_result == BOOT_UPGRADE_OK
      && (recovery.phase == BOOT_RECOVERY_CONFIRMED || recovery.phase == BOOT_RECOVERY_DONE))
    protected_slot = recovery.package.slot;
  char fields[320];
  (void)snprintf(fields, sizeof(fields),
      ",\"mode\":\"BOOT\",\"compat\":%u,\"normal\":%u,\"recovery\":%u,"
      "\"phase\":%u,\"sequence\":%lu,\"protected\":%lu,\"source\":%lu,"
      "\"tx_state\":%u,\"session_id\":%lu,\"next_offset\":%lu,\"total_len\":%lu",
      (unsigned int)UPGRADE_PERSISTENCE_VERSION, (unsigned int)normal_result,
      (unsigned int)recovery_result, (unsigned int)recovery.phase,
      (unsigned long)recovery.sequence, (unsigned long)protected_slot,
      (unsigned long)recovery.package.slot, (unsigned int)tx_result,
      (unsigned long)(session.active ? session.id : 0U),
      (unsigned long)session.offset, (unsigned long)session.total);
  (void)Reply(request, "OK", NULL, fields);
}

// 每个块明确返回持久读回后的偏移，重复查询/重复块不更新进展时间。
static bool ReplyProgress(const maint_frame_t *request)
{
  char fields[128];
  (void)snprintf(fields, sizeof(fields),
      ",\"session_id\":%lu,\"slot\":%lu,\"next_offset\":%lu,\"total_len\":%lu",
      (unsigned long)session.id, (unsigned long)session.slot,
      (unsigned long)session.offset, (unsigned long)session.total);
  return Reply(request, "OK", NULL, fields);
}

// 显式首次初始化或准备恢复槽；同一BEGIN重复请求不再擦槽。
static BootReceiveResult Begin(const maint_frame_t *request, const RecoveryArguments *args,
                               const uint8_t *public_key, const uint8_t *aes_key)
{
  if (args->present == ARG_INITIALIZE && args->initialize == 1U)
  {
    if (session.active) return Reject(request, "E_BUSY");
    Boot_UpgradeResult result = Boot_UpgradeInitializeTransactions();
    if (result != BOOT_UPGRADE_OK) return Reject(request, StorageError(result));
    (void)Reply(request, "OK", NULL, "");
    return BOOT_RECEIVE_COMMAND;
  }
  if (args->present != (ARG_SLOT | ARG_TOTAL)
      && args->present != (ARG_SLOT | ARG_TOTAL | ARG_ALLOW_UNKNOWN))
    return Reject(request, "E_REQUIRED");
  if ((args->slot != 1U && args->slot != 2U) || args->total_len < 216U
      || args->total_len > 448U * 1024U + 208U) return Reject(request, "E_RANGE");
  if (session.active)
  {
    if (session.begin_request_id != request->request_id) return Reject(request, "E_BUSY");
    if (args->slot != session.slot || args->total_len != session.total
        || (args->allow_unknown != 0U) != session.allow_unknown)
      return Reject(request, "E_INTEGRITY");
    (void)ReplyProgress(request);
    return BOOT_RECEIVE_COMMAND;
  }
  if (public_key == NULL || aes_key == NULL) return Reject(request, "E_AUTH");
  if (session.id != 0U && session.begin_request_id == request->request_id)
    return Reject(request, "E_EXPIRED");
  Boot_UpgradeResult result = Boot_RecoveryBegin(args->slot, args->total_len,
                                                args->allow_unknown != 0U);
  if (result != BOOT_UPGRADE_OK) return Reject(request, StorageError(result));
  // 复用持久事务号，避免重启后新会话再次接受旧会话的数据块。
  Boot_RecoveryState receiving;
  result = Boot_RecoveryLoad(&receiving);
  if (result != BOOT_UPGRADE_OK) return Reject(request, StorageError(result));
  if (receiving.tx_id == 0U || receiving.tx_id > UINT32_MAX)
    return Reject(request, "E_CAPACITY");
  uint32_t address = args->slot == 1U ? FLASH_PART_FW_A_BASE : FLASH_PART_FW_B_BASE;
  uint32_t started_ms = HAL_GetTick();
  for (uint32_t offset = 0U; offset < FLASH_PART_FW_A_END - FLASH_PART_FW_A_BASE; offset += 4096U)
  {
    if (!Boot_WatchdogCheck(started_ms, RECOVERY_ERASE_TIMEOUT_MS)
        || Boot_FlashRecoveryErase(address + offset) != HAL_OK
        || !Boot_WatchdogProgress(started_ms, RECOVERY_ERASE_TIMEOUT_MS))
      return Reject(request, Boot_WatchdogFailed() ? "E_TIMEOUT" : "E_STORAGE");
  }
  session.id = (uint32_t)receiving.tx_id;
  session.begin_request_id = request->request_id;
  session.slot = args->slot;
  session.total = args->total_len;
  session.offset = 0U;
  session.allow_unknown = args->allow_unknown != 0U;
  session.started_ms = HAL_GetTick();
  session.progress_ms = session.started_ms;
  session.active = true;
  (void)ReplyProgress(request);
  return BOOT_RECEIVE_COMMAND;
}

// CHUNK前8B为小端会话号和偏移，其余1～1016B为原始加密包字节。
static uint32_t Read32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8U)
       | ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

// 顺序写入跨页块；重复块逐段读回匹配，不重复编程、不续期。
static BootReceiveResult Chunk(const maint_frame_t *request)
{
  if (request->payload_len < 9U) return Reject(request, "E_RANGE");
  if (!session.active || Read32(request->payload) != session.id)
    return Reject(request, "E_MODE");
  uint32_t offset = Read32(request->payload + 4U);
  uint32_t length = request->payload_len - 8U;
  const uint8_t *data = request->payload + 8U;
  if (offset > session.offset || offset > session.total || length > session.total - offset)
    return Reject(request, "E_RANGE");
  bool duplicate = offset < session.offset;
  if (duplicate && length > session.offset - offset) return Reject(request, "E_RANGE");
  uint32_t address = (session.slot == 1U ? FLASH_PART_FW_A_BASE : FLASH_PART_FW_B_BASE) + offset;
  uint8_t check[256];
  for (uint32_t done = 0U; done < length;)
  {
    uint32_t count = 256U - ((address + done) % 256U);
    if (count > length - done) count = length - done;
    if (!Boot_WatchdogCheck(session.started_ms, RECOVERY_RECEIVE_TIMEOUT_MS))
    {
      session.active = false;
      return Reject(request, "E_TIMEOUT");
    }
    HAL_StatusTypeDef result = duplicate
        ? Boot_FlashRead(address + done, check, (uint16_t)count)
        : Boot_FlashRecoveryProgram(address + done, data + done, (uint16_t)count);
    if (result != HAL_OK)
    {
      session.active = false;
      return Reject(request, "E_STORAGE");
    }
    if (duplicate && memcmp(check, data + done, count) != 0)
      return Reject(request, "E_INTEGRITY");
    done += count;
  }
  if (!Boot_WatchdogCheck(session.started_ms, RECOVERY_RECEIVE_TIMEOUT_MS))
  {
    session.active = false;
    return Reject(request, "E_TIMEOUT");
  }
  if (!duplicate)
  {
    session.offset += length;
    session.progress_ms = HAL_GetTick();
  }
  (void)ReplyProgress(request);
  return BOOT_RECEIVE_COMMAND;
}

// 完整认证及READY持久提交后才交回main安装；ACCEPTED不表示新App已确认。
static BootReceiveResult Handle(const maint_frame_t *request,
                                const uint8_t *public_key, const uint8_t *aes_key)
{
  if (request->flags != 0U) return BOOT_RECEIVE_COMMAND;
  if (request->opcode == 0x31U) return Chunk(request);
  if (request->opcode != 0x01U && request->opcode != 0x02U && request->opcode != 0x16U
      && request->opcode != 0x30U && request->opcode != 0x32U)
    return Reject(request, "E_MODE");
  RecoveryArguments args;
  if (!ParseArguments(request, &args)) return Reject(request, "E_FORMAT");
  if (request->opcode == 0x30U) return Begin(request, &args, public_key, aes_key);
  if (request->opcode == 0x32U)
  {
    if (args.present != ARG_SESSION) return Reject(request, "E_REQUIRED");
    if (!session.active || args.session_id != session.id) return Reject(request, "E_MODE");
    if (session.offset != session.total) return Reject(request, "E_RANGE");
    session.active = false;
    Boot_UpgradeResult result = Boot_RecoveryAccept(public_key, aes_key);
    if (result != BOOT_UPGRADE_OK)
      return Reject(request, result == BOOT_UPGRADE_INVALID ? "E_AUTH" : StorageError(result));
    (void)Reply(request, "ACCEPTED", NULL, ",\"phase\":\"READY\"");
    last_request = *request;
    last_request.payload = NULL;
    return BOOT_RECEIVE_PACKAGE;
  }
  if (args.present != 0U) return Reject(request, "E_FORMAT");
  if (request->opcode == 0x16U)
  {
    if (session.active) return Reject(request, "E_BUSY");
    if (Reply(request, "OK", NULL, "")) NVIC_SystemReset();
    return BOOT_RECEIVE_COMMAND;
  }
  ReplyInfo(request);
  return BOOT_RECEIVE_COMMAND;
}

// 安装器只运行一次；回显FINISH的请求号，试运行开始不等于产品健康确认。
void Boot_RecoveryReplyResult(Boot_InstallResult result)
{
  char fields[96];
  (void)snprintf(fields, sizeof(fields), ",\"phase\":\"%s\",\"install_result\":%u",
                 result == BOOT_INSTALL_TRIAL_STARTED ? "TRIAL" : "ERROR", (unsigned int)result);
  (void)Reply(&last_request, result == BOOT_INSTALL_TRIAL_STARTED ? "OK" : "ERROR",
              result == BOOT_INSTALL_TRIAL_STARTED ? NULL : "E_STORAGE", fields);
}

// 固定10s请求窗口；半帧2s、收包30s无新增数据和180s总期限均不因重复输入续期。
BootReceiveResult Boot_RecoveryReceive(const uint8_t *public_key, const uint8_t *aes_key)
{
  uint32_t window_ms = HAL_GetTick();
  bool traffic = false;
  for (;;)
  {
    uint32_t now_ms = HAL_GetTick();
    if (Boot_WatchdogFailed()) return BOOT_RECEIVE_COMMAND;
    if (session.active && ((uint32_t)(now_ms - session.progress_ms) >= MAINT_FRAME_SESSION_TIMEOUT_MS
        || (uint32_t)(now_ms - session.started_ms) >= RECOVERY_RECEIVE_TIMEOUT_MS))
    {
      (void)Boot_WatchdogCheck(session.started_ms, RECOVERY_RECEIVE_TIMEOUT_MS);
      session.active = false;
      receive_size = 0U;
      return BOOT_RECEIVE_COMMAND;
    }
    if (receive_size != 0U && (uint32_t)(now_ms - partial_started_ms) >= MAINT_FRAME_PARTIAL_TIMEOUT_MS)
      receive_size = 0U;
    if ((uint32_t)(now_ms - window_ms) >= RECOVERY_REQUEST_WINDOW_MS)
      return traffic || session.active ? BOOT_RECEIVE_COMMAND : BOOT_RECEIVE_NO_REQUEST;
    if (receive_size != 0U)
    {
      maint_frame_t request;
      uint16_t consumed = 0U;
      maint_frame_result_t decoded = maint_frame_decode(receive_buffer, receive_size, &request, &consumed);
      if (decoded == MAINT_FRAME_OK)
      {
        traffic = true;
        BootReceiveResult result = Handle(&request, public_key, aes_key);
        receive_size = 0U;
        return result;
      }
      if (consumed != 0U)
      {
        receive_size -= consumed;
        memmove(receive_buffer, receive_buffer + consumed, receive_size);
        continue;
      }
    }
    uint32_t before_ms = HAL_GetTick();
    HAL_StatusTypeDef status = HAL_UART_Receive(&huart2, receive_buffer + receive_size, 1U, 50U);
    if (status != HAL_OK && status != HAL_TIMEOUT) { receive_size = 0U; return BOOT_RECEIVE_COMMAND; }
    if (status == HAL_TIMEOUT && HAL_GetTick() == before_ms) return BOOT_RECEIVE_COMMAND;
    if (status == HAL_OK)
    {
      if (receive_size == 0U) partial_started_ms = HAL_GetTick();
      receive_size++;
    }
    if (!Boot_WatchdogIdleProgress()) return BOOT_RECEIVE_COMMAND;
  }
}
