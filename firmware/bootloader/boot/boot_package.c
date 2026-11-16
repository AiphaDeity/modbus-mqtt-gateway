#include "boot_package.h"
#include "boot_watchdog.h"
#include "stm32f4xx_hal.h"

#include <stdio.h>

typedef struct
{
  uint32_t started_ms;
  uint32_t step_cycles;
  uint32_t max_cycles;
  uint32_t steps;
  bool cycles_available;
} Boot_PackageTiming;

// 为公共认证提供 HAL 单调毫秒时基。
static uint32_t NowMs(void *context)
{
  (void)context;
  return HAL_GetTick();
}

// 按是否取得实际进展，检查期限或刷新 Boot 看门狗。
static bool Checkpoint(void *context, uint32_t started_ms, uint32_t timeout_ms, bool progress)
{
  (void)context;
  return progress ? Boot_WatchdogProgress(started_ms, timeout_ms)
                  : Boot_WatchdogCheck(started_ms, timeout_ms);
}

// 记录验签失败的库错误码；调试构建额外采集分段周期数。
static void SignatureEvent(void *context, UpgradePackageSignatureEvent event, int result)
{
#if defined(DEBUG)
  Boot_PackageTiming *timing = context;
  switch (event)
  {
    case UPGRADE_PACKAGE_SIGNATURE_BEGIN:
      timing->started_ms = HAL_GetTick();
      timing->steps = 0U;
      timing->max_cycles = 0U;
      timing->cycles_available = Boot_WatchdogEnableCycleCounter();
      break;
    case UPGRADE_PACKAGE_SIGNATURE_STEP_BEGIN:
      timing->step_cycles = timing->cycles_available ? DWT->CYCCNT : 0U;
      break;
    case UPGRADE_PACKAGE_SIGNATURE_STEP_END:
    {
      uint32_t cycles = timing->cycles_available ? (uint32_t)(DWT->CYCCNT - timing->step_cycles) : 0U;
      timing->steps++;
      if (cycles > timing->max_cycles)
      {
        timing->max_cycles = cycles;
      }
      break;
    }
    case UPGRADE_PACKAGE_SIGNATURE_END:
      printf("WDG BUDGET ecdsa result=%d steps=%lu max_cycles=%lu cycles_ok=%u hclk_hz=%lu elapsed_ms=%lu\r\n",
             result, (unsigned long)timing->steps, (unsigned long)timing->max_cycles,
             (unsigned int)timing->cycles_available, (unsigned long)SystemCoreClock,
             (unsigned long)(HAL_GetTick() - timing->started_ms));
      break;
    default:
      break;
  }
#else
  (void)context;
  if ((event == UPGRADE_PACKAGE_SIGNATURE_END) && (result != 0))
  {
    printf("Package signature failed result=%d\r\n", result);
  }
#endif
}

// 建立使用 Boot 时基及监督回调的认证操作。
static void InitOperation(UpgradePackageOperation *operation, UpgradePackagePlatform *platform,
                           Boot_PackageTiming *timing)
{
  platform->context = timing;
  platform->now_ms = NowMs;
  platform->checkpoint = Checkpoint;
  platform->signature_event = SignatureEvent;
  operation->platform = platform;
  operation->started_ms = HAL_GetTick();
  operation->timeout_ms = UPGRADE_PACKAGE_TIMEOUT_MS;
  operation->error = UPGRADE_PACKAGE_ERROR_NONE;
}

// 调用共用格式检查，输出固件长度和未认证能力。
bool Boot_PackageCheckManifest(const uint8_t *manifest, size_t manifest_size, size_t package_size,
                              uint32_t *firmware_size, uint16_t *capabilities)
{
  return UpgradePackage_CheckManifest(manifest, manifest_size, package_size,
                                       firmware_size, capabilities);
}

// 在固定验签期限内校验摘要签名，保留密码库错误码。
int Boot_PackageVerifyDigestResult(const uint8_t *digest, const uint8_t *signature,
                                   const uint8_t *public_key)
{
  Boot_PackageTiming timing = {0};
  UpgradePackagePlatform platform;
  UpgradePackageOperation operation;
  InitOperation(&operation, &platform, &timing);
  return UpgradePackage_VerifyDigestResult(&operation, digest, signature, public_key);
}

// 将验签结果转换为成功或失败。
bool Boot_PackageVerifyDigest(const uint8_t *digest, const uint8_t *signature,
                             const uint8_t *public_key)
{
  return Boot_PackageVerifyDigestResult(digest, signature, public_key) == 0;
}

// 在 Boot 监督下读取包并完成签名验证。
bool Boot_PackageVerifySignature(Boot_PackageRead read, void *context, size_t package_size,
                                 const uint8_t *public_key, uint32_t *firmware_size)
{
  Boot_PackageTiming timing = {0};
  UpgradePackagePlatform platform;
  UpgradePackageOperation operation;
  InitOperation(&operation, &platform, &timing);
  return UpgradePackage_VerifySignature(&operation, read, context, package_size,
                                         public_key, firmware_size);
}

// 在 Boot 监督下验证签名、GCM 及明文摘要。
bool Boot_PackageVerify(Boot_PackageRead read, void *context, size_t package_size,
                        const uint8_t *public_key, const uint8_t *aes_key,
                        uint32_t *firmware_size)
{
  Boot_PackageTiming timing = {0};
  UpgradePackagePlatform platform;
  UpgradePackageOperation operation;
  InitOperation(&operation, &platform, &timing);
  return UpgradePackage_Verify(&operation, read, context, package_size,
                                public_key, aes_key, firmware_size);
}

// 在 Boot 监督下认证并二次解密输出固件。
bool Boot_PackageDecrypt(Boot_PackageRead read, void *read_context, size_t package_size,
                         const uint8_t *public_key, const uint8_t *aes_key,
                         Boot_PackageWrite write, void *write_context)
{
  Boot_PackageTiming timing = {0};
  UpgradePackagePlatform platform;
  UpgradePackageOperation operation;
  InitOperation(&operation, &platform, &timing);
  bool ok = UpgradePackage_Decrypt(&operation, read, read_context, package_size,
                                    public_key, aes_key, write, write_context);
  if (!ok)
  {
    printf("Package decrypt failed error=%u size=%lu\r\n",
           (unsigned int)operation.error, (unsigned long)package_size);
  }
  return ok;
}

// 在 Boot 主线程认证包及向量，沿用固定期限和分段进展监督。
bool Boot_PackageVerifyImage(Boot_PackageRead read, void *context, size_t package_size,
                              const uint8_t *public_key, const uint8_t *aes_key,
                              UpgradePackageImage *image)
{
  Boot_PackageTiming timing = {0};
  UpgradePackagePlatform platform;
  UpgradePackageOperation operation;
  InitOperation(&operation, &platform, &timing);
  bool ok = UpgradePackage_VerifyImage(&operation, read, context, package_size,
                                        public_key, aes_key, image);
  if (!ok)
  {
    printf("Package verify failed error=%u size=%lu\r\n",
           (unsigned int)operation.error, (unsigned long)package_size);
  }
  return ok;
}
