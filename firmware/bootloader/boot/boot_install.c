#include "boot_install.h"

#include "boot_app_flash.h"
#include "boot_upgrade_state.h"
#include "boot_trial.h"
#include "boot_recovery.h"
#include "boot_watchdog.h"
#include "flash_partition.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

#include <string.h>

#define APP_BASE 0x08010000U
#define APP_CAPACITY (448U * 1024U)
#define PACKAGE_OVERHEAD 208U
// 各阶段独立固定上限；不是整次启动或实板擦除耗时的承诺。
#define APP_HASH_TIMEOUT_MS 30000U
#define APP_ERASE_TIMEOUT_MS 30000U

typedef struct
{
  uint32_t offset; // 已完成片内写入的明文字节数，相对 App 起点。
  uint32_t used; // 当前工作缓冲已使用字节数，尚未计入 offset。
  uint8_t data[256]; // 只聚合一块明文，写完即清零，不保存整个固件。
} Boot_InstallWriter;

// 将日志包槽和长度转换为可读取的外部 Flash 源。
static bool GetSource(const Boot_UpgradePackage *package, Boot_PackageFlashSource *source)
{
  if ((package->slot < 1U) || (package->slot > 2U)
      || (package->size < PACKAGE_OVERHEAD + 8U) || (package->size > APP_CAPACITY + PACKAGE_OVERHEAD))
  {
    return false;
  }
  source->address = (package->slot == 1U) ? FLASH_PART_FW_A_BASE : FLASH_PART_FW_B_BASE;
  source->size = package->size;
  return true;
}

/*
 * 重新读取外部槽中的实际包，认证签名、GCM、明文摘要和启动向量，输出可信包描述及能力。
 * 此处只读检查，解密结果交给校验/向量收集回调，不擦写片内 App。
 */
bool Boot_InstallVerifySource(const Boot_PackageFlashSource *source, const uint8_t *public_key,
                          const uint8_t *aes_key, Boot_UpgradePackage *package,
                          uint16_t *capabilities)
{
  if (capabilities != NULL)
  {
    *capabilities = 0U;
  }
  if ((source == NULL) || (package == NULL) || Boot_WatchdogFailed()
      || ((source->address != FLASH_PART_FW_A_BASE) && (source->address != FLASH_PART_FW_B_BASE)))
  {
    return false;
  }
  Boot_PackageFlashSource snapshot = *source;
  UpgradePackageImage image;
  if (!Boot_PackageVerifyImage(Boot_PackageReadFlash, &snapshot, snapshot.size,
                                public_key, aes_key, &image))
  {
    return false;
  }
  package->slot = (source->address == FLASH_PART_FW_A_BASE) ? 1U : 2U;
  package->size = source->size;
  memcpy(package->digest, image.digest, sizeof(package->digest));
  if (capabilities != NULL)
  {
    *capabilities = image.capabilities;
  }
  return true;
}

/*
 * 认证持久记录引用的实际包：GetSource 映射槽和总长，Boot_InstallVerifySource 执行完整认证。
 * 最后比较的是已认证明文镜像摘要，不能用日志 CRC 或上一轮 App 的认证结果替代。
 */
static bool VerifyRecorded(const Boot_UpgradePackage *package, const uint8_t *public_key,
                            const uint8_t *aes_key, Boot_PackageFlashSource *source)
{
  Boot_UpgradePackage actual;
  return GetSource(package, source) && Boot_InstallVerifySource(source, public_key, aes_key, &actual, NULL)
      && (memcmp(actual.digest, package->digest, sizeof(actual.digest)) == 0);
}

// 只读检查片内真实 App 的向量和完整 SHA-256；覆盖实际固件长度，不含末尾为字对齐补入的 FF。
bool Boot_InstallAppMatches(const Boot_UpgradePackage *package)
{
  Boot_PackageFlashSource source;
  if ((package == NULL) || !GetSource(package, &source))
  {
    return false;
  }
  // 包格式中密文与明文等长，减去包头、签名和标签即得到真实 App 长度。
  uint32_t size = source.size - PACKAGE_OVERHEAD;
  uint32_t started_ms = HAL_GetTick();
  if (!Boot_WatchdogCheck(started_ms, APP_HASH_TIMEOUT_MS))
  {
    return false;
  }
  uint8_t data[256];
  uint8_t digest[32];
  if (!Boot_AppFlashRead(APP_BASE, data, 8U) || !UpgradePackage_CheckVector(data, size))
  {
    return false;
  }
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  bool ok = mbedtls_sha256_starts(&hash, 0) == 0;
  for (uint32_t offset = 0U; ok && (offset < size);)
  {
    uint32_t count = size - offset;
    if (count > sizeof(data))
    {
      count = sizeof(data);
    }
    ok = Boot_WatchdogCheck(started_ms, APP_HASH_TIMEOUT_MS)
      && Boot_AppFlashRead(APP_BASE + offset, data, count)
      && (mbedtls_sha256_update(&hash, data, count) == 0)
      && Boot_WatchdogProgress(started_ms, APP_HASH_TIMEOUT_MS);
    offset += count;
  }
  ok = ok && (mbedtls_sha256_finish(&hash, digest) == 0)
          && (memcmp(digest, package->digest, sizeof(digest)) == 0);
  mbedtls_sha256_free(&hash);
  mbedtls_platform_zeroize(data, sizeof(data));
  return Boot_WatchdogCheck(started_ms, APP_HASH_TIMEOUT_MS) && ok;
}

// 认证首次登记包并核对当前 App，建立已确认状态。
Boot_InstallResult Boot_InstallEnroll(const Boot_PackageFlashSource *source,
                                     const uint8_t *public_key, const uint8_t *aes_key, uint64_t tx_id)
{
  Boot_UpgradeState state;
  if (Boot_UpgradeLoad(&state) != BOOT_UPGRADE_EMPTY)
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  if (!Boot_InstallVerifySource(source, public_key, aes_key, &state.confirmed, NULL))
  {
    return BOOT_INSTALL_SOURCE_ERROR;
  }
  if (!Boot_InstallAppMatches(&state.confirmed))
  {
    return BOOT_INSTALL_APP_ERROR;
  }
  state.phase = BOOT_UPGRADE_CONFIRMED;
  state.tx_id = tx_id;
  state.operation = UPGRADE_OPERATION_ENROLL;
  return (Boot_UpgradeSave(&state) == BOOT_UPGRADE_OK)
       ? BOOT_INSTALL_BOOT_EXISTING : BOOT_INSTALL_STATE_ERROR;
}

// 刷出最多 256 B 明文工作缓冲，最后不足一个字以 FF 填充；容量不是片内 Flash 的编程粒度。
static bool FlushWriter(Boot_InstallWriter *writer)
{
  if (writer->used == 0U)
  {
    return true;
  }
  // 片内按 32 位字编程，最后不足 4 B 用 FF 填充；镜像摘要仍只覆盖原始长度。
  uint32_t size = (writer->used + 3U) & ~3U;
  memset(writer->data + writer->used, 0xFF, size - writer->used);
  // 先写本块再读回比较；它证明本块写入一致，整镜像摘要仍由安装结束时另行核对。
  bool ok = Boot_AppFlashProgram(APP_BASE + writer->offset, writer->data, size)
         && Boot_AppFlashVerify(APP_BASE + writer->offset, writer->data, size);
  if (ok)
  {
    writer->offset += writer->used;
    writer->used = 0U;
  }
  mbedtls_platform_zeroize(writer->data, sizeof(writer->data));
  return ok;
}

/*
 * Boot_PackageDecrypt 在输出明文时同步回调，offset 相对固件起点，不含包头、签名和标签。
 * data 只在本次回调期间借用，复制到安装工作缓冲；缓冲填满就经 FlushWriter 写入并读回。
 */
static bool WritePlaintext(void *context, size_t offset, const uint8_t *data, size_t size)
{
  Boot_InstallWriter *writer = context;
  if ((offset != writer->offset + writer->used) || (offset > APP_CAPACITY)
      || (size > APP_CAPACITY - offset))
  {
    return false;
  }
  while (size != 0U)
  {
    size_t count = sizeof(writer->data) - writer->used;
    if (count > size)
    {
      count = size;
    }
    memcpy(writer->data + writer->used, data, count);
    writer->used += (uint32_t)count;
    data += count;
    size -= count;
    if ((writer->used == sizeof(writer->data)) && !FlushWriter(writer))
    {
      return false;
    }
  }
  return true;
}

/*
 * 把已认证的外部包安装到唯一的片内 App 区，由普通安装或恢复流程调用。
 * 调用前须持久提交安装意图；返回 true 才表示解密写入及片内核对全部完成。
 * 失败可能已擦除或部分写入，调用方按持久状态恢复，不在片内从失败偏移续写。
 */
bool Boot_InstallImage(Boot_PackageFlashSource *source, const Boot_UpgradePackage *package,
                          const uint8_t *public_key, const uint8_t *aes_key)
{
  // 1. 核对包描述与实际读取源，避免拿一个槽的身份去安装另一个槽的内容。
  Boot_PackageFlashSource expected;
  if ((source == NULL) || (package == NULL) || (public_key == NULL) || (aes_key == NULL)
      || !GetSource(package, &expected) || (expected.address != source->address)
      || (expected.size != source->size))
  {
    return false;
  }
  // 2. 从头擦除 App 区，给后续明文写入准备空白空间；阶段超时由 Boot 监督处理。
  uint32_t erase_started_ms = HAL_GetTick();
  // 目标仅为 [0x08010000, 0x08080000) 的 448 KiB，固定使用扇区 4～7。
  // Boot 及其末尾信任记录位于前面的区域，不参与此处擦除。
  for (uint32_t sector = FLASH_SECTOR_4; sector <= FLASH_SECTOR_7; sector++)
  {
    if (!Boot_WatchdogCheck(erase_started_ms, APP_ERASE_TIMEOUT_MS)
        || !Boot_AppFlashEraseSector(sector)
        || !Boot_WatchdogProgress(erase_started_ms, APP_ERASE_TIMEOUT_MS))
    {
      return false;
    }
  }
  // 3. Boot_PackageDecrypt 完整认证后再次解密，明文经 WritePlaintext → FlushWriter 写入片内。
  // 这里只复用固定块缓冲，包仍留在外部槽；不在 RAM 保存完整 App。
  // 最后刷出不足一块的数据，再用 Boot_InstallAppMatches 核对片内实际镜像。
  Boot_InstallWriter writer = {0};
  bool ok = Boot_PackageDecrypt(Boot_PackageReadFlash, source, source->size,
                                public_key, aes_key, WritePlaintext, &writer)
         && FlushWriter(&writer) && Boot_InstallAppMatches(package);
  mbedtls_platform_zeroize(&writer, sizeof(writer));
  return ok;
}

/*
 * 普通升级不能继续时恢复 confirmed 旧包，由 Boot_InstallRun 的失败分支调用。
 * 先认证旧源，再持久记录 RESTORING，重装成功后恢复 CONFIRMED；结果交回 main 判断启动。
 * 旧源或重装检查失败时返回错误，保留持久状态供后续维护或重新启动处理，不放行损坏 App。
 */
static Boot_InstallResult Restore(Boot_UpgradeState *state, const uint8_t *public_key,
                                  const uint8_t *aes_key)
{
  if (Boot_WatchdogFailed())
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  // 1. 旧包也必须重新认证，只有日志中的槽号和摘要还不足以开始恢复。
  Boot_PackageFlashSource source;
  if (!VerifyRecorded(&state->confirmed, public_key, aes_key, &source))
  {
    return BOOT_INSTALL_SOURCE_ERROR;
  }
  // 2. 先持久记录恢复意图，再擦写 App；断电后根据 RESTORING 从头恢复同一旧包。
  if (state->phase != BOOT_UPGRADE_RESTORING)
  {
    if ((state->phase == BOOT_UPGRADE_CONFIRMED)
        && (Boot_UpgradeReserveTransaction(&state->tx_id) != BOOT_UPGRADE_OK))
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
    state->operation = UPGRADE_OPERATION_RESTORE;
    state->phase = BOOT_UPGRADE_RESTORING;
    if (Boot_UpgradeSave(state) != BOOT_UPGRADE_OK)
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
  }
  // 3. 复用同一安装器，把旧包重新写入片内 App 区，不切换片内执行地址。
  if (!Boot_InstallImage(&source, &state->confirmed, public_key, aes_key))
  {
    // RESTORING 仍留在日志中，下次 Boot 从旧包重新完整安装，不从中断字节位置续写。
    return BOOT_INSTALL_APP_ERROR;
  }
  // 4. 恢复成功才清候选引用并保存 CONFIRMED；保存成功后返回 RESTORED。
  state->phase = BOOT_UPGRADE_CONFIRMED;
  memset(&state->candidate, 0, sizeof(state->candidate));
  return (Boot_UpgradeSave(state) == BOOT_UPGRADE_OK)
       ? BOOT_INSTALL_RESTORED : BOOT_INSTALL_STATE_ERROR;
}

/*
 * Boot main 取得可信密钥后调用本状态机，按记录决定本次启动动作。
 * 先处理独立恢复事务，再处理普通试运行、旧包恢复、正常启动或候选安装。
 * 普通升级主线为 READY → INSTALLING → TRIAL；确认后转 CONFIRMED，失败则走 RESTORING 分支。
 * 返回的是处理结果；main 还要处理维护窗口和最终认证，之后才可能跳转 App。
 */
Boot_InstallResult Boot_InstallRun(const uint8_t *public_key, const uint8_t *aes_key)
{
  // 1. 已锁存监督失败时停止推进状态，main 收到返回后会执行故障复位。
  if (Boot_WatchdogFailed())
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  // 2. 优先让 Boot_RecoveryRun 处理独立恢复事务；只有 NO_RECOVERY 才继续看普通日志。
  Boot_InstallResult recovery = Boot_RecoveryRun(public_key, aes_key);
  if (recovery != BOOT_INSTALL_NO_RECOVERY)
  {
    return recovery;
  }
  // 3. 读取普通升级记录；空白或读取失败都不能凭现有 App 向量建立可信启动依据。
  // 掉电会丢失 RAM 进度；这里只采用已提交日志，不扫描 A/B 槽把残留半包自行登记为候选。
  Boot_UpgradeState state;
  Boot_UpgradeResult result = Boot_UpgradeLoad(&state);
  if (result == BOOT_UPGRADE_EMPTY)
  {
    // 没有可信启动依据，留在串口恢复入口；首次建档仍由显式请求完成。
    return BOOT_INSTALL_STATE_ERROR;
  }
  if (result != BOOT_UPGRADE_OK)
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  // 4. TRIAL 且收到匹配确认：核对包与片内镜像，再把候选正式提升为已确认包。
  if ((state.phase == BOOT_UPGRADE_TRIAL) && Boot_TrialConfirmationMatches(&state))
  {
    // 邮箱只表达健康确认意图，仍须重新认证候选包并核对片内实际内容。
    Boot_PackageFlashSource candidate;
    if (!VerifyRecorded(&state.candidate, public_key, aes_key, &candidate)
        || !Boot_InstallAppMatches(&state.candidate))
    {
      return Restore(&state, public_key, aes_key);
    }
    // 邮箱、候选源包和实际 App 均匹配后，才把候选提升为已确认包。
    // 这里只更新包描述的角色；candidate 清零不会擦除刚提升的外部包，也不会再次重装片内 App。
    // 保存成功才完成正式接收；仅 RAM 中改为 CONFIRMED 或收到确认邮箱，都不是持久确认。
    state.confirmed = state.candidate;
    memset(&state.candidate, 0, sizeof(state.candidate));
    state.phase = BOOT_UPGRADE_CONFIRMED;
    return (Boot_UpgradeSave(&state) == BOOT_UPGRADE_OK)
         ? BOOT_INSTALL_BOOT_EXISTING : BOOT_INSTALL_STATE_ERROR;
  }
  if (state.phase == BOOT_UPGRADE_TRIAL)
  {
    // 仍为 TRIAL 表示没有有效确认；还有机会且源包、镜像都有效，才允许再次试运行。
    // 上限包含安装后的首次放行；这里仅在原次数为 1 且检查通过时持久记录第 2 次机会。
    if (state.trial_attempts < UPGRADE_TRIAL_LIMIT)
    {
      Boot_PackageFlashSource candidate;
      Boot_PackageFlashSource confirmed;
      if (VerifyRecorded(&state.confirmed, public_key, aes_key, &confirmed)
          && VerifyRecorded(&state.candidate, public_key, aes_key, &candidate)
          && Boot_InstallAppMatches(&state.candidate))
      {
        // 先持久消耗本次机会，main 才能交接；这不是在此处执行跳转。
        state.trial_attempts++;
        return Boot_UpgradeSave(&state) == BOOT_UPGRADE_OK
             ? BOOT_INSTALL_TRIAL_STARTED : BOOT_INSTALL_STATE_ERROR;
      }
    }
    // 次数耗尽或内容不匹配，恢复原已确认包。
    return Restore(&state, public_key, aes_key);
  }
  if (state.phase == BOOT_UPGRADE_RESTORING)
  {
    // 5. 上次恢复未完成：沿已保存的 RESTORING 意图，从头恢复旧包。
    return Restore(&state, public_key, aes_key);
  }
  Boot_PackageFlashSource old_source;
  // 6. 下面处理 CONFIRMED、READY 或 INSTALLING；先认证旧包，确保它仍可作为恢复源。
  if (!VerifyRecorded(&state.confirmed, public_key, aes_key, &old_source))
  {
    return BOOT_INSTALL_SOURCE_ERROR;
  }
  if (state.phase == BOOT_UPGRADE_CONFIRMED)
  {
    // CONFIRMED：片内与可信旧包一致就返回可启动，否则先恢复；正常上电不一定擦写。
    // READY 提交前中断仍走此分支，未被日志引用的候选槽数据不会取得安装资格。
    return Boot_InstallAppMatches(&state.confirmed) ? BOOT_INSTALL_BOOT_EXISTING
                                      : Restore(&state, public_key, aes_key);
  }
  // 7. READY/INSTALLING 表示有候选待处理，认证失败则走普通旧包恢复。
  Boot_PackageFlashSource candidate;
  if (!VerifyRecorded(&state.candidate, public_key, aes_key, &candidate))
  {
    return Restore(&state, public_key, aes_key);
  }
  if (state.phase == BOOT_UPGRADE_READY)
  {
    // READY 尚未动片内 App，先确认旧程序仍正确，再持久推进 INSTALLING。
    if (!Boot_InstallAppMatches(&state.confirmed))
    {
      return Restore(&state, public_key, aes_key);
    }
    // READY 还没有擦片内；INSTALLING 落盘后才进入实际擦除和编程。
    state.phase = BOOT_UPGRADE_INSTALLING;
    if (Boot_UpgradeSave(&state) != BOOT_UPGRADE_OK)
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
  }
  // READY 已推进 INSTALLING，或上次已在 INSTALLING 中断，都从头安装同一候选包。
  // INSTALLING 下片内可能只有部分镜像，不能因向量看似有效就把它当作正常启动。
  // 即使上次已写完整 App、只是 TRIAL 尚未提交，也仍重新认证并从头擦除安装，不续用 RAM 偏移。
  if (!Boot_InstallImage(&candidate, &state.candidate, public_key, aes_key))
  {
    return Restore(&state, public_key, aes_key);
  }
  // 8. 安装核对成功，记录首次 TRIAL 机会；后续由 App 运行健康判定并写确认邮箱。
  // 返回 TRIAL_STARTED 后仍回到 main，由其准备启动邮箱、检查交接条件并跳转。
  state.phase = BOOT_UPGRADE_TRIAL;
  state.trial_attempts = 1U;
  // 先持久消耗放行机会，才允许后续邮箱和跳转；记录后掉电也计入次数。
  // TRIAL 的实际提交点决定下次按安装还是试运行处理，不能只凭当前 RAM 阶段或最后一行日志判断。
  return (Boot_UpgradeSave(&state) == BOOT_UPGRADE_OK)
       ? BOOT_INSTALL_TRIAL_STARTED : BOOT_INSTALL_STATE_ERROR;
}
