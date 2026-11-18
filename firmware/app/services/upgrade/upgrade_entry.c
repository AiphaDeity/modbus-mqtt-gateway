#include "upgrade_entry.h"

#include "stm32f4xx_hal.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

/*
 * 网络和 TF 共用一个入口；先核对来源授权，再交给 Stage 推进接收状态。
 * owner 用来源和 epoch 区分一次操作，session_id 则由 Stage 分配，三者须匹配。
 */
static UpgradeEntryPolicy policy;
static bool policy_bound;
static bool entry_busy; // 只覆盖当前接口调用，长操作期间拒绝其他修改请求。
static bool owned; // 跨接口保留会话归属，不因一次 Write 返回就解除。
static UpgradeEntryPeer owner;
static uint32_t owner_session;
// 连接断开即锁存，包括Stage尚未分配会话号的BEGIN准备期。
static bool owner_cancelled;

// 尝试占用升级入口，拒绝并发修改操作。
static UpgradeStageResult Take(void)
{
  if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning))
  {
    return UPGRADE_STAGE_MODE;
  }
  taskENTER_CRITICAL();
  UpgradeStageResult result = !policy_bound ? UPGRADE_STAGE_NOT_READY
                            : entry_busy ? UPGRADE_STAGE_BUSY : UPGRADE_STAGE_OK;
  if (result == UPGRADE_STAGE_OK)
  {
    entry_busy = true;
  }
  taskEXIT_CRITICAL();
  return result;
}

// 释放升级入口的修改占用。
static UpgradeStageResult Release(UpgradeStageResult result)
{
  taskENTER_CRITICAL();
  entry_busy = false;
  taskEXIT_CRITICAL();
  return result;
}

// 检查来源和 epoch，并调用已绑定的权限策略。
static bool Authorized(const UpgradeEntryPeer *peer)
{
  return (peer != NULL) && (peer->epoch != 0U)
      && ((peer->source == UPGRADE_STAGE_SOURCE_NETWORK) || (peer->source == UPGRADE_STAGE_SOURCE_TF))
      && policy.authorize(policy.context, peer);
}

// 调用方持有entry_busy或处于临界区，保证owner身份不会同时改变。
static bool SamePeer(const UpgradeEntryPeer *peer)
{
  return owned && (peer != NULL) && (peer->source == owner.source) && (peer->epoch == owner.epoch);
}

// 检查请求是否来自当前会话的所有者。
static bool SameOwner(const UpgradeEntryPeer *peer, uint32_t session_id)
{
  return SamePeer(peer) && (session_id == owner_session) && (session_id != 0U);
}

// 每次 Write/Finish 等操作都复查当前授权和 source/epoch/session 归属，Begin 成功不是永久许可。
// 成功后由调用方 Release；失败在这里释放，不能再调用 Stage 修改接口。
static UpgradeStageResult TakeSession(const UpgradeEntryPeer *peer, uint32_t session_id)
{
  UpgradeStageResult result = Take();
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (!Authorized(peer) || !SameOwner(peer, session_id))
  {
    return Release(UPGRADE_STAGE_MODE);
  }
  return UPGRADE_STAGE_OK;
}

// 在临界区更新入口所有者、会话号及取消状态。
static void SetOwner(const UpgradeEntryPeer *peer, uint32_t session_id)
{
  taskENTER_CRITICAL();
  if (peer == NULL)
  {
    owned = false;
    owner_session = 0U;
    owner_cancelled = false;
  }
  else
  {
    bool new_owner = !SamePeer(peer);
    owner = *peer;
    owner_session = session_id;
    owned = true;
    if (new_owner || (session_id == 0U))
    {
      owner_cancelled = false;
    }
  }
  taskEXIT_CRITICAL();
}

// 已持有entry_busy；Stage确认旧会话已结束，Begin随后才替换连接归属。
static UpgradeStageResult RetirePreviousSession(const UpgradeEntryPeer *peer)
{
  UpgradeStageStatus status;
  UpgradeStageResult result = UpgradeStage_GetStatus(&status);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (status.busy || (status.phase == UPGRADE_STAGE_QUIESCING)
      || (status.phase == UPGRADE_STAGE_REBOOT_PENDING))
  {
    return UPGRADE_STAGE_BUSY;
  }
  bool needs_cleanup = status.cleanup_pending || (status.resume_error != UPGRADE_STAGE_OK);
  if (!needs_cleanup && ((status.phase == UPGRADE_STAGE_IDLE) || (status.phase == UPGRADE_STAGE_ERROR)))
  {
    return UPGRADE_STAGE_OK;
  }
  taskENTER_CRITICAL();
  bool disconnected = owned && owner_cancelled;
  taskEXIT_CRITICAL();
  if (!disconnected && !(SamePeer(peer) && needs_cleanup))
  {
    return UPGRADE_STAGE_BUSY;
  }
  result = UpgradeStage_Abort(status.session_id);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  result = UpgradeStage_GetStatus(&status);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (status.busy || status.cleanup_pending || (status.resume_error != UPGRADE_STAGE_OK)
      || ((status.phase != UPGRADE_STAGE_IDLE) && (status.phase != UPGRADE_STAGE_ERROR)))
  {
    return UPGRADE_STAGE_BUSY;
  }
  return UPGRADE_STAGE_OK;
}

// 将升级结果码转换为协议使用的静态错误名称。
const char *UpgradeEntry_ErrorName(UpgradeStageResult result)
{
  switch (result)
  {
    case UPGRADE_STAGE_OK: return NULL;
    case UPGRADE_STAGE_INVALID: return "E_FORMAT";
    case UPGRADE_STAGE_OFFSET: return "E_RANGE";
    case UPGRADE_STAGE_MODE: return "E_MODE";
    case UPGRADE_STAGE_BUSY: return "E_BUSY";
    case UPGRADE_STAGE_NOT_READY:
    case UPGRADE_STAGE_STORAGE: return "E_STORAGE";
    case UPGRADE_STAGE_INTEGRITY: return "E_INTEGRITY";
    case UPGRADE_STAGE_AUTH: return "E_AUTH";
    case UPGRADE_STAGE_TIMEOUT: return "E_TIMEOUT";
    default: return "E_INTERNAL";
  }
}

// 将升级阶段转换为状态响应中的静态名称。
const char *UpgradeEntry_PhaseName(UpgradeStagePhase phase)
{
  switch (phase)
  {
    case UPGRADE_STAGE_IDLE: return "IDLE";
    case UPGRADE_STAGE_STAGING: return "STAGING";
    case UPGRADE_STAGE_VERIFIED: return "VERIFIED";
    case UPGRADE_STAGE_QUIESCING: return "QUIESCING";
    case UPGRADE_STAGE_REBOOT_PENDING: return "REBOOT_PENDING";
    default: return "ERROR";
  }
}

// 校验并保存唯一权限策略绑定。
UpgradeStageResult UpgradeEntry_BindPolicy(const UpgradeEntryPolicy *binding)
{
  if ((binding == NULL) || (binding->authorize == NULL))
  {
    return UPGRADE_STAGE_INVALID;
  }
  if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning))
  {
    return UPGRADE_STAGE_MODE;
  }
  taskENTER_CRITICAL();
  if (policy_bound)
  {
    taskEXIT_CRITICAL();
    return UPGRADE_STAGE_BUSY;
  }
  policy = *binding;
  policy_bound = true;
  taskEXIT_CRITICAL();
  return UPGRADE_STAGE_OK;
}

// 锁存指定操作的断线/撤权，并尝试取消和清理其会话。
UpgradeStageResult UpgradeEntry_Disconnect(const UpgradeEntryPeer *peer)
{
  if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning))
  {
    return UPGRADE_STAGE_MODE;
  }
  taskENTER_CRITICAL();
  uint32_t session_id = owner_session;
  bool matches = SamePeer(peer);
  if (matches)
  {
    owner_cancelled = true;
    // 只置Stage标志，无I/O；与断线锁存原子衔接，不能让Finish抢先提交。
    (void)UpgradeStage_RequestAbort(session_id);
  }
  taskEXIT_CRITICAL();
  if (!matches)
  {
    return UPGRADE_STAGE_MODE;
  }
  // 不等入口慢操作锁，先让进行中的接收/认证在下一个检查点取消。
  if (session_id == 0U)
  {
    UpgradeStageStatus status;
    if ((UpgradeStage_GetStatus(&status) == UPGRADE_STAGE_OK) && (status.source == peer->source))
    {
      taskENTER_CRITICAL();
      // 查询期间旧BEGIN可能已退出；不能把随后新连接的会话当成取消目标。
      if (SamePeer(peer))
      {
        session_id = owner_session != 0U ? owner_session : status.session_id;
      }
      taskEXIT_CRITICAL();
    }
    (void)UpgradeStage_RequestAbort(session_id);
  }
  UpgradeStageResult result = Take();
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (!SamePeer(peer))
  {
    return Release(UPGRADE_STAGE_MODE);
  }
  result = UpgradeStage_Abort(session_id);
  if (result == UPGRADE_STAGE_OK)
  {
    SetOwner(NULL, 0U);
  }
  return Release(result);
}

/*
 * TF 或网络执行任务调用的接收入口：先校验来源授权，再请求 Stage 建立候选会话。
 * Begin → Write → Finish → Reboot 共用本次 peer/session；Entry 管归属，Stage 管进度和提交。
 */
UpgradeStageResult UpgradeEntry_Begin(const UpgradeEntryPeer *peer, const char *path,
                                      uint32_t total_size, const uint8_t *sha256, uint32_t *session_id)
{
  if (session_id == NULL)
  {
    return UPGRADE_STAGE_INVALID;
  }
  *session_id = 0U;
  UpgradeStageResult result = Take();
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  if (!Authorized(peer))
  {
    return Release(UPGRADE_STAGE_MODE);
  }
  bool tf = peer->source == UPGRADE_STAGE_SOURCE_TF;
  // TF 必须来自固定文件，网络由下载器提供字节流，所以网络 path 必须为 NULL。
  if ((tf && ((path == NULL) || (strcmp(path, UPGRADE_TF_PATH) != 0))) || (!tf && (path != NULL)))
  {
    return Release(UPGRADE_STAGE_INVALID);
  }
  result = RetirePreviousSession(peer);
  if (result != UPGRADE_STAGE_OK)
  {
    return Release(result);
  }
  // 先登记归属，让撤权通知能锁存取消；清理旧会话后再核对本次权限。
  SetOwner(peer, 0U);
  if (!Authorized(peer))
  {
    SetOwner(NULL, 0U);
    return Release(UPGRADE_STAGE_MODE);
  }
  // 权限及旧会话清理通过后才进入 Stage；Stage 经后端准备候选槽，并输出新的会话号。
  result = UpgradeStage_Begin(peer->source, total_size, sha256, session_id);
  if (result == UPGRADE_STAGE_OK)
  {
    taskENTER_CRITICAL();
    owner_session = *session_id; // 发布会话号和检查断线原子衔接，不重置断线标志。
    bool cancelled = owner_cancelled;
    taskEXIT_CRITICAL();
    if (cancelled)
    {
      (void)UpgradeStage_RequestAbort(*session_id);
      result = UpgradeStage_Abort(*session_id);
      if (result == UPGRADE_STAGE_OK)
      {
        SetOwner(NULL, 0U);
      }
      *session_id = 0U;
      return Release(result == UPGRADE_STAGE_OK ? UPGRADE_STAGE_MODE : result);
    }
  }
  else
  {
    // BEGIN应答失败也可能留下待取消会话，仍绑定原通道，不允许另一个连接接管。
    UpgradeStageStatus status;
    UpgradeStageResult queried = UpgradeStage_GetStatus(&status);
    if (queried != UPGRADE_STAGE_OK)
    {
      return Release(result); // 保留原始失败及owner，不能把查询失败当作已清理。
    }
    if (status.busy || status.cleanup_pending || (status.resume_error != UPGRADE_STAGE_OK)
        || ((status.phase != UPGRADE_STAGE_IDLE) && (status.phase != UPGRADE_STAGE_ERROR)))
    {
      SetOwner(peer, status.session_id);
    }
    else
    {
      SetOwner(NULL, 0U);
    }
  }
  return Release(result);
}

// 核对归属和会话后，将数据块交给接收服务。
UpgradeStageResult UpgradeEntry_Write(const UpgradeEntryPeer *peer, uint32_t session_id,
                                      uint32_t offset, const uint8_t *data, uint32_t size)
{
  UpgradeStageResult result = TakeSession(peer, session_id);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  // 输入数据只在同步调用期间借给 Stage；它写入并核对完成后，传输任务才能复用本块缓冲。
  return Release(UpgradeStage_Write(session_id, offset, data, size));
}

/*
 * TF 或网络收齐包后调用：UpgradeStage_Finish 只认证到 VERIFIED。
 * 随后由 UpgradeStage_Commit 暂停业务并提交 READY。
 * 两步之间重新检查来源许可；OK 表示候选安装请求已持久化，不能据此宣称 App 已安装或转正。
 */
UpgradeStageResult UpgradeEntry_Finish(const UpgradeEntryPeer *peer, uint32_t session_id)
{
  UpgradeStageResult result = TakeSession(peer, session_id);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  UpgradeStageStatus status;
  result = UpgradeStage_GetStatus(&status);
  if (result == UPGRADE_STAGE_OK)
  {
    if (status.phase == UPGRADE_STAGE_STAGING)
    {
      // 先完成整包认证；VERIFIED 只表示认证成功，尚未持久提交 READY。
      result = UpgradeStage_Finish(session_id);
    }
    if (result == UPGRADE_STAGE_OK)
    {
      // 认证可能耗时；提交前复查权限，撤权后由原调用方完成取消结算。
      if (!Authorized(peer))
      {
        (void)UpgradeStage_RequestAbort(session_id);
        result = UPGRADE_STAGE_MODE;
      }
      else
      {
        // Commit 才执行业务暂停、在途结算及 READY 提交；不明确结果不能按失败重发。
        result = UpgradeStage_Commit(session_id);
      }
    }
  }
  return Release(result);
}

// 取消归属匹配的会话，清理完成后释放入口归属。
UpgradeStageResult UpgradeEntry_Abort(const UpgradeEntryPeer *peer, uint32_t session_id)
{
  UpgradeStageResult result = TakeSession(peer, session_id);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  result = UpgradeStage_Abort(session_id);
  if (result == UPGRADE_STAGE_OK)
  {
    SetOwner(NULL, 0U);
  }
  return Release(result);
}

// 核对归属后，请求已提交会话执行复位。
UpgradeStageResult UpgradeEntry_Reboot(const UpgradeEntryPeer *peer, uint32_t session_id)
{
  UpgradeStageResult result = TakeSession(peer, session_id);
  if (result != UPGRADE_STAGE_OK)
  {
    return result;
  }
  result = UpgradeStage_Reboot(session_id);
  return Release(result);
}

// 校验来源权限后读取当前升级状态快照。
UpgradeStageResult UpgradeEntry_GetStatus(const UpgradeEntryPeer *peer, UpgradeStageStatus *status)
{
  // 查询不占慢操作标志，认证/Flash等待期间仍能看到完整已发布快照。
  if (status == NULL)
  {
    return UPGRADE_STAGE_INVALID;
  }
  memset(status, 0, sizeof(*status));
  if ((__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning))
  {
    return UPGRADE_STAGE_MODE;
  }
  taskENTER_CRITICAL();
  bool bound = policy_bound;
  taskEXIT_CRITICAL();
  if (!bound)
  {
    return UPGRADE_STAGE_NOT_READY;
  }
  if (!Authorized(peer))
  {
    return UPGRADE_STAGE_MODE;
  }
  return UpgradeStage_GetStatus(status);
}
