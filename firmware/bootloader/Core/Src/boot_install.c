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
  uint32_t offset;
  uint32_t used;
  uint8_t data[256];
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

// 一次认证流程同时检查源包、解密结果和向量，避免调用方重复跑同一套检查。
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

// 认证日志引用的包，并核对已记录的长度和摘要。
static bool VerifyRecorded(const Boot_UpgradePackage *package, const uint8_t *public_key,
                            const uint8_t *aes_key, Boot_PackageFlashSource *source)
{
  Boot_UpgradePackage actual;
  return GetSource(package, source) && Boot_InstallVerifySource(source, public_key, aes_key, &actual, NULL)
      && (memcmp(actual.digest, package->digest, sizeof(actual.digest)) == 0);
}

// 用有界读取接口散列片内真实内容，摘要不包含最后一个字的FF填充。
bool Boot_InstallAppMatches(const Boot_UpgradePackage *package)
{
  Boot_PackageFlashSource source;
  if ((package == NULL) || !GetSource(package, &source))
  {
    return false;
  }
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
#if UPGRADE_PERSISTENCE_VERSION == 2
  state.tx_id = tx_id;
  state.operation = UPGRADE_OPERATION_ENROLL;
#else
  (void)tx_id;
#endif
  return (Boot_UpgradeSave(&state) == BOOT_UPGRADE_OK)
       ? BOOT_INSTALL_BOOT_EXISTING : BOOT_INSTALL_STATE_ERROR;
}

// 聚合到256字节；最后不足一个字以FF填充，禁止越过App末端。
static bool FlushWriter(Boot_InstallWriter *writer)
{
  if (writer->used == 0U)
  {
    return true;
  }
  uint32_t size = (writer->used + 3U) & ~3U;
  memset(writer->data + writer->used, 0xFF, size - writer->used);
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

// 接收连续明文块，填充页缓冲并触发片内写入。
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

// 仅在安装/恢复状态已提交且源包已认证后调用；失败不在片内局部续写。
bool Boot_InstallImage(Boot_PackageFlashSource *source, const Boot_UpgradePackage *package,
                          const uint8_t *public_key, const uint8_t *aes_key)
{
  Boot_PackageFlashSource expected;
  if ((source == NULL) || (package == NULL) || (public_key == NULL) || (aes_key == NULL)
      || !GetSource(package, &expected) || (expected.address != source->address)
      || (expected.size != source->size))
  {
    return false;
  }
  uint32_t erase_started_ms = HAL_GetTick();
  for (uint32_t sector = FLASH_SECTOR_4; sector <= FLASH_SECTOR_7; sector++)
  {
    if (!Boot_WatchdogCheck(erase_started_ms, APP_ERASE_TIMEOUT_MS)
        || !Boot_AppFlashEraseSector(sector)
        || !Boot_WatchdogProgress(erase_started_ms, APP_ERASE_TIMEOUT_MS))
    {
      return false;
    }
  }
  Boot_InstallWriter writer = {0};
  bool ok = Boot_PackageDecrypt(Boot_PackageReadFlash, source, source->size,
                                public_key, aes_key, WritePlaintext, &writer)
         && FlushWriter(&writer) && Boot_InstallAppMatches(package);
  mbedtls_platform_zeroize(&writer, sizeof(writer));
  return ok;
}

// 重新认证旧包，先提交恢复状态，再从头安装；全部成功后才清除候选引用。
static Boot_InstallResult Restore(Boot_UpgradeState *state, const uint8_t *public_key,
                                  const uint8_t *aes_key)
{
  if (Boot_WatchdogFailed())
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  Boot_PackageFlashSource source;
  if (!VerifyRecorded(&state->confirmed, public_key, aes_key, &source))
  {
    return BOOT_INSTALL_SOURCE_ERROR;
  }
  if (state->phase != BOOT_UPGRADE_RESTORING)
  {
#if UPGRADE_PERSISTENCE_VERSION == 2
    if ((state->phase == BOOT_UPGRADE_CONFIRMED)
        && (Boot_UpgradeReserveTransaction(&state->tx_id) != BOOT_UPGRADE_OK))
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
    state->operation = UPGRADE_OPERATION_RESTORE;
#endif
    state->phase = BOOT_UPGRADE_RESTORING;
    if (Boot_UpgradeSave(state) != BOOT_UPGRADE_OK)
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
  }
  if (!Boot_InstallImage(&source, &state->confirmed, public_key, aes_key))
  {
    return BOOT_INSTALL_APP_ERROR;
  }
  state->phase = BOOT_UPGRADE_CONFIRMED;
  memset(&state->candidate, 0, sizeof(state->candidate));
  return (Boot_UpgradeSave(state) == BOOT_UPGRADE_OK)
       ? BOOT_INSTALL_RESTORED : BOOT_INSTALL_STATE_ERROR;
}

// 依据持久状态推进安装、试运行确认或旧包恢复。
Boot_InstallResult Boot_InstallRun(const uint8_t *public_key, const uint8_t *aes_key)
{
  if (Boot_WatchdogFailed())
  {
    return BOOT_INSTALL_STATE_ERROR;
  }
  // 独立恢复事务优先于普通升级日志，不能因普通日志损坏/空白而绕过恢复屏障。
  Boot_InstallResult recovery = Boot_RecoveryRun(public_key, aes_key);
  if (recovery != BOOT_INSTALL_NO_RECOVERY)
  {
    return recovery;
  }
  // 先恢复事务；读失败不能当作首次空白启动。
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
  if ((state.phase == BOOT_UPGRADE_TRIAL) && Boot_TrialConfirmationMatches(&state))
  {
    // 邮箱只表达健康确认意图，仍须重新认证候选包并核对片内实际内容。
    Boot_PackageFlashSource candidate;
    if (!VerifyRecorded(&state.candidate, public_key, aes_key, &candidate)
        || !Boot_InstallAppMatches(&state.candidate))
    {
      return Restore(&state, public_key, aes_key);
    }
    state.confirmed = state.candidate;
    memset(&state.candidate, 0, sizeof(state.candidate));
    state.phase = BOOT_UPGRADE_CONFIRMED;
    return (Boot_UpgradeSave(&state) == BOOT_UPGRADE_OK)
         ? BOOT_INSTALL_BOOT_EXISTING : BOOT_INSTALL_STATE_ERROR;
  }
  if (state.phase == BOOT_UPGRADE_TRIAL)
  {
#if UPGRADE_PERSISTENCE_VERSION == 2
    if (state.trial_attempts < UPGRADE_TRIAL_LIMIT)
    {
      Boot_PackageFlashSource candidate;
      Boot_PackageFlashSource confirmed;
      if (VerifyRecorded(&state.confirmed, public_key, aes_key, &confirmed)
          && VerifyRecorded(&state.candidate, public_key, aes_key, &candidate)
          && Boot_InstallAppMatches(&state.candidate))
      {
        state.trial_attempts++;
        return Boot_UpgradeSave(&state) == BOOT_UPGRADE_OK
             ? BOOT_INSTALL_TRIAL_STARTED : BOOT_INSTALL_STATE_ERROR;
      }
    }
#endif
    return Restore(&state, public_key, aes_key);
  }
  if (state.phase == BOOT_UPGRADE_RESTORING)
  {
    return Restore(&state, public_key, aes_key);
  }
  Boot_PackageFlashSource old_source;
  // 安装候选前先确认旧包可恢复，不能只相信日志中的摘要。
  if (!VerifyRecorded(&state.confirmed, public_key, aes_key, &old_source))
  {
    return BOOT_INSTALL_SOURCE_ERROR;
  }
  if (state.phase == BOOT_UPGRADE_CONFIRMED)
  {
    return Boot_InstallAppMatches(&state.confirmed) ? BOOT_INSTALL_BOOT_EXISTING
                                      : Restore(&state, public_key, aes_key);
  }
  Boot_PackageFlashSource candidate;
  if (!VerifyRecorded(&state.candidate, public_key, aes_key, &candidate))
  {
    return Restore(&state, public_key, aes_key);
  }
  if (state.phase == BOOT_UPGRADE_READY)
  {
    // READY表示尚未擦写，当前App必须仍与已确认恢复包一致。
    if (!Boot_InstallAppMatches(&state.confirmed))
    {
      return Restore(&state, public_key, aes_key);
    }
    state.phase = BOOT_UPGRADE_INSTALLING;
    if (Boot_UpgradeSave(&state) != BOOT_UPGRADE_OK)
    {
      return BOOT_INSTALL_STATE_ERROR;
    }
  }
  if (!Boot_InstallImage(&candidate, &state.candidate, public_key, aes_key))
  {
    return Restore(&state, public_key, aes_key);
  }
  state.phase = BOOT_UPGRADE_TRIAL;
#if UPGRADE_PERSISTENCE_VERSION == 2
  state.trial_attempts = 1U;
#endif
  // 先持久消耗放行机会，才允许后续邮箱和跳转；记录后掉电也计入次数。
  return (Boot_UpgradeSave(&state) == BOOT_UPGRADE_OK)
       ? BOOT_INSTALL_TRIAL_STARTED : BOOT_INSTALL_STATE_ERROR;
}
