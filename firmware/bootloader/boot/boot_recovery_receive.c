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

// 单帧接收工作区；解码出的 payload 借用此缓冲，Handle 必须在缓冲复用前同步处理完。
static uint8_t receive_buffer[MAINT_FRAME_OVERHEAD + MAINT_FRAME_PAYLOAD_MAX];
static uint16_t receive_size;
static uint32_t partial_started_ms;
static maint_frame_t last_request;

static struct
{
  bool active;
  uint32_t id; // 从持久恢复事务号派生，重启后旧 CHUNK 不能认领新会话。
  uint32_t begin_request_id;
  uint32_t slot;
  uint32_t total; // 预期加密包总长度，单位 B。
  uint32_t offset; // 已新增写入的连续字节数，重复块只核对，不推进。
  uint32_t started_ms;
  uint32_t progress_ms; // 最后新增数据的 HAL 时刻，查询和重复请求不会续期。
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
      // 位图同时记录字段是否出现；重复同名字段也拒绝，避免前后值含义不一致。
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

/*
 * INFO/STATUS 只读返回普通日志、独立恢复日志、事务高水位和本轮 RAM 收包进度。
 * 未配置密钥时仍可诊断；查询不建恢复事务、不擦 Flash，保护槽还须在 BEGIN 执行时重新核对。
 */
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

/*
 * 处理 BEGIN：显式首次初始化单独走初始化分支，普通恢复则先建立屏障/RECEIVING 再擦选定槽。
 * 本函数建立 RAM 收包会话并返回起始偏移，不认证整包，也不擦写片内 App。
 * 活动会话内只有请求号和参数均相同的重复 BEGIN 才复用当前进度，不重新擦槽。
 */
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
  // 1. 内层检查保护槽并先写恢复屏障及 RECEIVING；成功后才有资格擦除所选候选槽。
  Boot_UpgradeResult result = Boot_RecoveryBegin(args->slot, args->total_len,
                                                args->allow_unknown != 0U);
  if (result != BOOT_UPGRADE_OK) return Reject(request, StorageError(result));
  // 复用持久事务号，避免重启后新会话再次接受旧会话的数据块。
  Boot_RecoveryState receiving;
  result = Boot_RecoveryLoad(&receiving);
  if (result != BOOT_UPGRADE_OK) return Reject(request, StorageError(result));
  if (receiving.tx_id == 0U || receiving.tx_id > UINT32_MAX)
    return Reject(request, "E_CAPACITY");
  // 2. 在固定期限内擦除指定外部包槽，保留可信旧槽和片内 App；此时尚不接受 CHUNK。
  uint32_t address = args->slot == 1U ? FLASH_PART_FW_A_BASE : FLASH_PART_FW_B_BASE;
  uint32_t started_ms = HAL_GetTick();
  for (uint32_t offset = 0U; offset < FLASH_PART_FW_A_END - FLASH_PART_FW_A_BASE; offset += 4096U)
  {
    if (!Boot_WatchdogCheck(started_ms, RECOVERY_ERASE_TIMEOUT_MS)
        || Boot_FlashRecoveryErase(address + offset) != HAL_OK
        || !Boot_WatchdogProgress(started_ms, RECOVERY_ERASE_TIMEOUT_MS))
      return Reject(request, Boot_WatchdogFailed() ? "E_TIMEOUT" : "E_STORAGE");
  }
  // 3. 全槽擦除成功才发布 RAM 会话；电脑收到 session_id 和 next_offset=0 后开始传块。
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

// 从 CHUNK 载荷读取一个小端 32 位字段，前两个字段依次为会话号和包内字节偏移。
static uint32_t Read32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8U)
       | ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

/*
 * 接收当前会话的原始 .gwp 字节；前 8 B 为会话号和包内偏移，其余为 1～1016 B 数据。
 * 连续新块按 Flash 页边界拆分并写入读回；完全位于已写范围的重复块只比较，不编程或续期。
 * payload 借用本次串口帧缓冲，仅在同步处理期间使用；本阶段不解密、不覆盖片内 App。
 */
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
    // 页编程不能跨 256 B 边界，串口块可跨页，因此逐段拆分后再访问 Flash。
    uint32_t count = 256U - ((address + done) % 256U);
    if (count > length - done) count = length - done;
    if (!Boot_WatchdogCheck(session.started_ms, RECOVERY_RECEIVE_TIMEOUT_MS))
    {
      session.active = false;
      return Reject(request, "E_TIMEOUT");
    }
    // 新块经 Boot_FlashRecoveryProgram 校验写权限、编程并读回；重复块直接读取已有数据比较。
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
    // 全块持久读回一致后才推进 RAM 进度；恢复日志仍为 RECEIVING，尚未提交 READY。
    session.offset += length;
    session.progress_ms = HAL_GetTick();
  }
  (void)ReplyProgress(request);
  return BOOT_RECEIVE_COMMAND;
}

/*
 * 在 Boot 内分发维护命令，不进入 App 的 Entry/Stage 或 Storage 任务。
 * FINISH 先核对收齐，再认证并持久 READY，回复 ACCEPTED 后把安装工作交回 main。
 */
static BootReceiveResult Handle(const maint_frame_t *request,
                                const uint8_t *public_key, const uint8_t *aes_key)
{
  if (request->flags != 0U) return BOOT_RECEIVE_COMMAND;
  // 0x01/0x02 查询，0x16 请求复位重判，0x30 BEGIN，0x31 CHUNK，0x32 FINISH。
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
    // 收齐只表示具备认证前提；失败后仍保留独立恢复记录和屏障，不能按已收字节直接启动。
    session.active = false;
    Boot_UpgradeResult result = Boot_RecoveryAccept(public_key, aes_key);
    if (result != BOOT_UPGRADE_OK)
      return Reject(request, result == BOOT_UPGRADE_INVALID ? "E_AUTH" : StorageError(result));
    // 同一 FINISH 的第一条应答：只报告 READY 已落盘；main 完成安装后还会回复第二条阶段结果。
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

/*
 * Boot main 执行本次恢复安装后调用，使用原 FINISH 请求号发送第二条应答。
 * OK/TRIAL 表示片内安装检查完成且试运行机会已登记；最终交接及 App 健康确认尚在后续。
 */
void Boot_RecoveryReplyResult(Boot_InstallResult result)
{
  char fields[96];
  (void)snprintf(fields, sizeof(fields), ",\"phase\":\"%s\",\"install_result\":%u",
                 result == BOOT_INSTALL_TRIAL_STARTED ? "TRIAL" : "ERROR", (unsigned int)result);
  (void)Reply(&last_request, result == BOOT_INSTALL_TRIAL_STARTED ? "OK" : "ERROR",
              result == BOOT_INSTALL_TRIAL_STARTED ? NULL : "E_STORAGE", fields);
}

/*
 * Boot main 在维护窗口同步调用，USART2 逐字节收帧，解码成功后 Handle 在缓冲复用前处理完。
 * 每次调用使用固定 10 s 窗口；半帧 2 s、收包无新增数据 30 s 及总期限 180 s 另行检查。
 * 返回命令/超时后仍由 main 判断是否驻留维护，不能把窗口结束当作恢复事务已完成。
 */
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
      // 只终止 RAM 会话并丢弃半帧，持久屏障和半包保留，不能按偏移自动续传。
      session.active = false;
      receive_size = 0U;
      return BOOT_RECEIVE_COMMAND;
    }
    if (receive_size != 0U && (uint32_t)(now_ms - partial_started_ms) >= MAINT_FRAME_PARTIAL_TIMEOUT_MS)
      receive_size = 0U;
    if ((uint32_t)(now_ms - window_ms) >= RECOVERY_REQUEST_WINDOW_MS)
      return traffic || session.active ? BOOT_RECEIVE_COMMAND : BOOT_RECEIVE_NO_REQUEST;
    // 完整帧才分发命令；MORE 保留半帧，格式/CRC 错误仅丢掉解码器确认可消费的前缀。
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
      // 错帧只丢解码器明确消费的前缀，保留后续字节重新寻找合法帧头。
      if (consumed != 0U)
      {
        receive_size -= consumed;
        memmove(receive_buffer, receive_buffer + consumed, receive_size);
        continue;
      }
    }
    // 本次只接收一个字节，下一轮重新尝试解码；USART1 日志不参与维护帧收发。
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
