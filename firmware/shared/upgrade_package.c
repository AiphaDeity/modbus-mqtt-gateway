#include "upgrade_package.h"

#include <string.h>
#include <stdlib.h>
#include "mbedtls/constant_time.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/gcm.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

// V2沿用固定布局，在原保留区声明能力，与发布工具保持一致。
#define UPGRADE_MANIFEST_SIZE UPGRADE_PACKAGE_MANIFEST_SIZE
#define UPGRADE_SIGNATURE_SIZE 64U
#define UPGRADE_PACKAGE_CHUNK_SIZE 256U
// 软件保护上限，非性能实测；整次认证/解密共用起点，不按块续期。
#define UPGRADE_SIGNATURE_TIMEOUT_MS UPGRADE_PACKAGE_SIGNATURE_TIMEOUT_MS
// 库的基本运算数，不是毫秒保证；实际时序由平台汇总，仍须实板确认。
#define UPGRADE_SIGNATURE_MAX_OPS 256U

// 硬件标识字段固定 16 B，名称后的剩余字节补零。
static const uint8_t package_hardware_id[16] = "F407-TEST";

// 只记录操作的首次失败，避免清理错误覆盖原始原因。
static bool Fail(UpgradePackageOperation *operation, UpgradePackageError error)
{
  if ((operation != NULL) && (operation->error == UPGRADE_PACKAGE_ERROR_NONE))
  {
    operation->error = error;
  }
  return false;
}

// 通过平台回调取得单调毫秒时基。
static uint32_t NowMs(const UpgradePackageOperation *operation)
{
  return operation->platform->now_ms(operation->platform->context);
}

// 检查操作参数及总期限，并通知平台处理进展。
static bool CheckOperation(UpgradePackageOperation *operation, bool progress)
{
  if ((operation == NULL) || (operation->platform == NULL)
      || (operation->platform->now_ms == NULL) || (operation->platform->checkpoint == NULL)
      || (operation->timeout_ms == 0U) || (operation->timeout_ms > 0x7FFFFFFFU))
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_INVALID);
  }
  const UpgradePackagePlatform *platform = operation->platform;
  bool within = (uint32_t)(NowMs(operation) - operation->started_ms) < operation->timeout_ms;
  bool ok = platform->checkpoint(platform->context, operation->started_ms,
                                  operation->timeout_ms, progress && within);
  return (within && ok) || Fail(operation, UPGRADE_PACKAGE_ERROR_TIMEOUT);
}

// 检查验签阶段及整次操作的固定期限。
static bool CheckSignatureDeadline(UpgradePackageOperation *operation,
                                    uint32_t started_ms, bool progress)
{
  if (!CheckOperation(operation, false))
  {
    return false;
  }
  const UpgradePackagePlatform *platform = operation->platform;
  bool within = (uint32_t)(NowMs(operation) - started_ms) < UPGRADE_SIGNATURE_TIMEOUT_MS;
  bool ok = platform->checkpoint(platform->context, started_ms,
                                  UPGRADE_SIGNATURE_TIMEOUT_MS, progress && within);
  return (within && ok) || Fail(operation, UPGRADE_PACKAGE_ERROR_TIMEOUT);
}

// 向可选的诊断回调报告验签阶段和结果。
static void SignatureEvent(UpgradePackageOperation *operation,
                            UpgradePackageSignatureEvent event, int result)
{
  const UpgradePackagePlatform *platform = operation->platform;
  if (platform->signature_event != NULL)
  {
    platform->signature_event(platform->context, event, result);
  }
}

// 在操作期限内读取包数据，记录首次读取失败。
static bool ReadPackage(UpgradePackageOperation *operation, UpgradePackageRead read,
                         void *context, size_t offset, uint8_t *data, size_t size)
{
  if (!CheckOperation(operation, false))
  {
    return false;
  }
  if ((read == NULL) || !read(context, offset, data, size))
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_IO);
  }
  return CheckOperation(operation, false);
}

// 在操作期限内输出明文块，失败后停止后续处理。
static bool WritePackage(UpgradePackageOperation *operation, UpgradePackageWrite write,
                          void *context, size_t offset, const uint8_t *data, size_t size)
{
  if (!CheckOperation(operation, false))
  {
    return false;
  }
  if (!write(context, offset, data, size))
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_IO);
  }
  return CheckOperation(operation, false);
}

// 从可读缓冲解码小端整数，避免未对齐访问。
static uint16_t ReadLe16(const uint8_t *data)
{
  return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

// 从字节缓冲解码小端 32 位整数。
static uint32_t ReadLe32(const uint8_t *data)
{
  return (uint32_t)data[0]
       | ((uint32_t)data[1] << 8U)
       | ((uint32_t)data[2] << 16U)
       | ((uint32_t)data[3] << 24U);
}

// 先验证缓冲边界，再按协议偏移读取字段，最后输出可用的固件长度。
bool UpgradePackage_CheckManifest(const uint8_t *manifest,
                              size_t manifest_size,
                              size_t package_size,
                              uint32_t *firmware_size,
                              uint16_t *capabilities)
{
  if (capabilities != NULL)
  {
    *capabilities = 0U;
  }
  if (firmware_size == NULL)
  {
    return false;
  }
  *firmware_size = 0U;

  if ((manifest == NULL) || (manifest_size != UPGRADE_MANIFEST_SIZE)
      || (package_size < UPGRADE_PACKAGE_OVERHEAD))
  {
    return false;
  }

  if ((memcmp(manifest, "GWUPD001", 8U) != 0)
      || (ReadLe16(&manifest[10]) != UPGRADE_MANIFEST_SIZE)
      || (memcmp(&manifest[12], package_hardware_id, sizeof(package_hardware_id)) != 0)
      || (ReadLe32(&manifest[32]) != UPGRADE_PACKAGE_APP_ADDRESS))
  {
    return false;
  }

  uint16_t version = ReadLe16(&manifest[8]);
  uint16_t checked_capabilities = ReadLe16(&manifest[114]);
  // 这里只检查格式准入，通过后仍须完整认证才能获得安装或启动资格。
  if ((version != 2U)
      || (checked_capabilities != UPGRADE_PACKAGE_CAP_IWDG_HANDOFF))
  {
    return false;
  }

  uint32_t plaintext_size = ReadLe32(&manifest[36]);
  uint32_t ciphertext_size = ReadLe32(&manifest[40]);
  // 已检查总长下限，用减法比较，避免包头中的不可信长度导致加法溢出。
  if ((plaintext_size == 0U) || (plaintext_size > UPGRADE_PACKAGE_APP_CAPACITY)
      || (ciphertext_size != plaintext_size)
      || (ciphertext_size != package_size - UPGRADE_PACKAGE_OVERHEAD))
  {
    return false;
  }

  // 只接受编号 1 的签名和解密密钥；实际密钥由可信映射提供。
  if ((ReadLe32(&manifest[44]) != 1U)
      || (ReadLe32(&manifest[108]) != 1U)
      || (ReadLe16(&manifest[112]) != UPGRADE_PERSISTENCE_VERSION))
  {
    return false;
  }
  for (size_t i = 116U; i < UPGRADE_MANIFEST_SIZE; i++)
  {
    if (manifest[i] != 0U)
    {
      return false;
    }
  }

  *firmware_size = plaintext_size;
  if (capabilities != NULL)
  {
    *capabilities = checked_capabilities;
  }
  return true;
}

// 同一上下文逐步验签；所有出口释放对象，返回原库错误码供自测区分失败原因。
static int UpgradePackage_CheckDigest(const uint8_t *digest,
                                   const uint8_t *signature,
                                   const uint8_t *public_key, UpgradePackageOperation *operation)
{
  if ((digest == NULL) || (signature == NULL) || (public_key == NULL)
      || (public_key[0] != 0x04U) || !CheckOperation(operation, false))
  {
    (void)Fail(operation, UPGRADE_PACKAGE_ERROR_INVALID);
    return MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
  }
  uint32_t started_ms = NowMs(operation);
  SignatureEvent(operation, UPGRADE_PACKAGE_SIGNATURE_BEGIN, 0);
  mbedtls_ecdsa_restart_ctx restart;
  mbedtls_ecdsa_restart_init(&restart);
  mbedtls_ecp_group group;
  mbedtls_ecp_point public_key_point;
  mbedtls_mpi r;
  mbedtls_mpi s;

  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&public_key_point);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);

  int result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
  if (result == 0)
  {
    result = mbedtls_ecp_point_read_binary(&group, &public_key_point, public_key, 65U);
  }
  if (result == 0)
  {
    result = mbedtls_mpi_read_binary(&r, signature, 32U);
  }
  if (result == 0)
  {
    result = mbedtls_mpi_read_binary(&s, signature + 32U, 32U);
  }
  if (result == 0)
  {
    mbedtls_ecp_set_max_ops(UPGRADE_SIGNATURE_MAX_OPS);
    do
    {
      if (!CheckSignatureDeadline(operation, started_ms, false))
      {
        result = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
        break;
      }
      SignatureEvent(operation, UPGRADE_PACKAGE_SIGNATURE_STEP_BEGIN, 0);
      result = mbedtls_ecdsa_verify_restartable(&group, digest, 32U,
                                                &public_key_point, &r, &s, &restart);
      SignatureEvent(operation, UPGRADE_PACKAGE_SIGNATURE_STEP_END, result);
      if (!CheckSignatureDeadline(operation, started_ms, false))
      {
        result = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
        break;
      }
      if ((result == MBEDTLS_ERR_ECP_IN_PROGRESS)
          && !CheckSignatureDeadline(operation, started_ms, true))
      {
        result = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
        break;
      }
    } while (result == MBEDTLS_ERR_ECP_IN_PROGRESS);
    mbedtls_ecp_set_max_ops(0U);
  }

  mbedtls_ecdsa_restart_free(&restart);
  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_ecp_point_free(&public_key_point);
  mbedtls_ecp_group_free(&group);
  SignatureEvent(operation, UPGRADE_PACKAGE_SIGNATURE_END, result);
  if (result != 0)
  {
    (void)Fail(operation, ((result == MBEDTLS_ERR_ECP_VERIFY_FAILED)
                           || (result == MBEDTLS_ERR_ECP_BAD_INPUT_DATA))
                          ? UPGRADE_PACKAGE_ERROR_AUTH : UPGRADE_PACKAGE_ERROR_INTERNAL);
  }
  if (!CheckSignatureDeadline(operation, started_ms, false))
  {
    return MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
  }
  return result;
}

// 分段执行 P-256 验签，返回密码库结果并释放工作区。
int UpgradePackage_VerifyDigestResult(UpgradePackageOperation *operation,
                                      const uint8_t *digest, const uint8_t *signature,
                                      const uint8_t *public_key)
{
  return UpgradePackage_CheckDigest(digest, signature, public_key, operation);
}

// 包头先做边界检查，再跳过签名区域，连续散列密文和标签。
static bool UpgradePackage_CheckSignature(UpgradePackageRead read, void *context,
                                       size_t package_size, const uint8_t *public_key,
                                       uint32_t *firmware_size, UpgradePackageOperation *operation)
{
  if (firmware_size == NULL)
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_INVALID);
  }
  *firmware_size = 0U;
  if ((read == NULL) || (public_key == NULL) || (public_key[0] != 0x04U)
      || (package_size <= UPGRADE_PACKAGE_OVERHEAD)
      || (package_size - UPGRADE_PACKAGE_OVERHEAD > UPGRADE_PACKAGE_APP_CAPACITY)
      || !CheckOperation(operation, false))
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_INVALID);
  }

  uint8_t manifest[UPGRADE_MANIFEST_SIZE];
  uint8_t signature[UPGRADE_SIGNATURE_SIZE];
  uint32_t checked_size = 0U;
  if (!ReadPackage(operation, read, context, 0U, manifest, sizeof(manifest))
      || !UpgradePackage_CheckManifest(manifest, sizeof(manifest), package_size, &checked_size, NULL)
      || !ReadPackage(operation, read, context, UPGRADE_MANIFEST_SIZE, signature, sizeof(signature)))
  {
    (void)CheckOperation(operation, false);
    return Fail(operation, UPGRADE_PACKAGE_ERROR_INVALID);
  }

  uint8_t buffer[UPGRADE_PACKAGE_CHUNK_SIZE];
  uint8_t digest[32];
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  int result = mbedtls_sha256_starts(&hash, 0);
  if (result == 0)
  {
    result = mbedtls_sha256_update(&hash, manifest, sizeof(manifest));
  }

  size_t offset = UPGRADE_MANIFEST_SIZE + UPGRADE_SIGNATURE_SIZE;
  bool read_ok = true;
  while ((result == 0) && (offset < package_size))
  {
    size_t size = package_size - offset;
    if (size > sizeof(buffer))
    {
      size = sizeof(buffer);
    }
    if (!CheckOperation(operation, false)
        || !ReadPackage(operation, read, context, offset, buffer, size))
    {
      read_ok = false;
      break;
    }
    result = mbedtls_sha256_update(&hash, buffer, size);
    if ((result == 0)
        && !CheckOperation(operation, true))
    {
      read_ok = false;
      break;
    }
    offset += size;
  }
  if ((result == 0) && read_ok)
  {
    result = mbedtls_sha256_finish(&hash, digest);
  }
  mbedtls_sha256_free(&hash);

  if (!CheckOperation(operation, false)
      || (result != 0) || !read_ok)
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_INTERNAL);
  }
  bool verified = UpgradePackage_CheckDigest(digest, signature, public_key, operation) == 0;
  if (!CheckOperation(operation, false) || !verified
      || !CheckOperation(operation, true))
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_AUTH);
  }
  *firmware_size = checked_size;
  return true;
}

// 验证包头并对包摘要验签，成功输出固件长度。
bool UpgradePackage_VerifySignature(UpgradePackageOperation *operation,
                                    UpgradePackageRead read, void *context, size_t package_size,
                                    const uint8_t *public_key, uint32_t *firmware_size)
{
  return UpgradePackage_CheckSignature(read, context, package_size, public_key,
                                       firmware_size, operation);
}

/*
 * 解密工作区按调用分配，避免 GCM 上下文和双缓冲叠加占用 Boot 启动栈。
 * 不保存完整固件，明文只用于摘要计算，返回前清零并释放整个工作区。
 */
typedef struct
{
  mbedtls_gcm_context gcm;
  mbedtls_sha256_context hash;
  uint8_t manifest[UPGRADE_MANIFEST_SIZE];
  uint8_t ciphertext[UPGRADE_PACKAGE_CHUNK_SIZE];
  uint8_t plaintext[UPGRADE_PACKAGE_CHUNK_SIZE + 15U];
  uint8_t expected_tag[16];
  uint8_t actual_tag[16];
  uint8_t digest[32];
} UpgradePackage_DecryptState;

// 已验签的包再次按块读取，只在标签、长度和明文摘要全部正确时接受。
static bool UpgradePackage_CheckPlaintext(UpgradePackageRead read, void *context,
                                       size_t package_size, const uint8_t *aes_key,
                                       uint32_t firmware_size, UpgradePackageWrite write,
                                       void *write_context, UpgradePackageOperation *operation)
{
  if (!CheckOperation(operation, false))
  {
    return false;
  }
  UpgradePackage_DecryptState *state = calloc(1U, sizeof(*state));
  if (state == NULL)
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_INTERNAL);
  }
  mbedtls_gcm_init(&state->gcm);
  mbedtls_sha256_init(&state->hash);
  bool verified = false;
  uint32_t checked_size = 0U;
  if (!ReadPackage(operation, read, context, 0U, state->manifest, sizeof(state->manifest))
      || !UpgradePackage_CheckManifest(state->manifest, sizeof(state->manifest),
                                    package_size, &checked_size, NULL)
      || (checked_size != firmware_size)
      || !ReadPackage(operation, read, context, package_size - sizeof(state->expected_tag),
               state->expected_tag, sizeof(state->expected_tag)))
  {
    (void)Fail(operation, UPGRADE_PACKAGE_ERROR_INVALID);
    goto cleanup;
  }

  // 包头原始字节作为 AAD，nonce 位于偏移 48，明文摘要位于偏移 60。
  if ((mbedtls_gcm_setkey(&state->gcm, MBEDTLS_CIPHER_ID_AES, aes_key, 256U) != 0)
      || (mbedtls_gcm_starts(&state->gcm, MBEDTLS_GCM_DECRYPT,
                             &state->manifest[48], 12U) != 0)
      || (mbedtls_gcm_update_ad(&state->gcm, state->manifest,
                                sizeof(state->manifest)) != 0)
      || (mbedtls_sha256_starts(&state->hash, 0) != 0))
  {
    (void)Fail(operation, UPGRADE_PACKAGE_ERROR_INTERNAL);
    goto cleanup;
  }

  size_t remaining = firmware_size;
  size_t offset = UPGRADE_MANIFEST_SIZE + UPGRADE_SIGNATURE_SIZE;
  size_t plaintext_size = 0U;
  while (remaining > 0U)
  {
    size_t size = remaining;
    if (size > sizeof(state->ciphertext))
    {
      size = sizeof(state->ciphertext);
    }
    size_t output_size = 0U;
    if (!CheckOperation(operation, false)
        || !ReadPackage(operation, read, context, offset, state->ciphertext, size)
        || (mbedtls_gcm_update(&state->gcm, state->ciphertext, size,
                                state->plaintext, sizeof(state->plaintext),
                                &output_size) != 0)
        || (output_size > firmware_size - plaintext_size)
        || (mbedtls_sha256_update(&state->hash, state->plaintext, output_size) != 0))
    {
      (void)Fail(operation, UPGRADE_PACKAGE_ERROR_INTERNAL);
      goto cleanup;
    }
    if (!CheckOperation(operation, false))
    {
      goto cleanup;
    }
    if ((write != NULL) && (output_size != 0U)
        && !WritePackage(operation, write, write_context, plaintext_size, state->plaintext, output_size))
    {
      goto cleanup;
    }
    plaintext_size += output_size;
    mbedtls_platform_zeroize(state->plaintext, sizeof(state->plaintext));
    offset += size;
    remaining -= size;
    if (!CheckOperation(operation, true))
    {
      goto cleanup;
    }
  }

  // finish 可能补出最后不足 16 字节的明文，必须计入长度和摘要。
  size_t output_size = 0U;
  if ((mbedtls_gcm_finish(&state->gcm, state->plaintext, sizeof(state->plaintext),
                           &output_size, state->actual_tag, sizeof(state->actual_tag)) != 0)
      || (output_size > firmware_size - plaintext_size)
      || (mbedtls_sha256_update(&state->hash, state->plaintext, output_size) != 0)
      || (plaintext_size + output_size != firmware_size)
      || (mbedtls_sha256_finish(&state->hash, state->digest) != 0))
  {
    (void)Fail(operation, UPGRADE_PACKAGE_ERROR_INTERNAL);
    goto cleanup;
  }
  // 常量时间比较，避免按第一个不同字节提前结束比较。
  if (mbedtls_ct_memcmp(state->actual_tag, state->expected_tag, 16U) != 0)
  {
    (void)Fail(operation, UPGRADE_PACKAGE_ERROR_AUTH);
  }
  else if (mbedtls_ct_memcmp(state->digest, &state->manifest[60], 32U) != 0)
  {
    (void)Fail(operation, UPGRADE_PACKAGE_ERROR_INTEGRITY);
  }
  else
  {
    verified = true;
  }
  if (!CheckOperation(operation, false))
  {
    verified = false;
    goto cleanup;
  }
  if (verified && (write != NULL) && (output_size != 0U))
  {
    verified = WritePackage(operation, write, write_context, plaintext_size, state->plaintext, output_size);
  }

cleanup:
  mbedtls_sha256_free(&state->hash);
  mbedtls_gcm_free(&state->gcm);
  mbedtls_platform_zeroize(state, sizeof(*state));
  free(state);
  bool within_deadline = CheckOperation(operation, false);
  if (!verified)
  {
    (void)Fail(operation, UPGRADE_PACKAGE_ERROR_INTERNAL);
  }
  return verified && within_deadline;
}

// 先认证包的来源和密文，再检查解密结果，不向外输出尚未认证的明文。
static bool UpgradePackage_Authenticate(UpgradePackageRead read,
                        void *context,
                        size_t package_size,
                        const uint8_t *public_key,
                        const uint8_t *aes_key,
                        uint32_t *firmware_size, UpgradePackageOperation *operation)
{
  if (firmware_size == NULL)
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_INVALID);
  }
  *firmware_size = 0U;
  uint32_t checked_size = 0U;
  if ((aes_key == NULL)
      || !UpgradePackage_CheckSignature(read, context, package_size, public_key,
                                      &checked_size, operation)
      || !UpgradePackage_CheckPlaintext(read, context, package_size, aes_key,
                                      checked_size, NULL, NULL, operation))
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_INVALID);
  }
  *firmware_size = checked_size;
  return true;
}

// 认证签名、GCM 标签及明文摘要，不输出固件明文。
bool UpgradePackage_Verify(UpgradePackageOperation *operation,
                           UpgradePackageRead read, void *context, size_t package_size,
                           const uint8_t *public_key, const uint8_t *aes_key,
                           uint32_t *firmware_size)
{
  return UpgradePackage_Authenticate(read, context, package_size, public_key,
                                     aes_key, firmware_size, operation);
}

// 先完整认证，再次解密并输出明文，同时复核完整性。
bool UpgradePackage_Decrypt(UpgradePackageOperation *operation,
                            UpgradePackageRead read, void *read_context, size_t package_size,
                            const uint8_t *public_key, const uint8_t *aes_key,
                            UpgradePackageWrite write, void *write_context)
{
  uint32_t firmware_size = 0U;
  if (write == NULL)
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_INVALID);
  }
  return UpgradePackage_Authenticate(read, read_context, package_size, public_key,
                                      aes_key, &firmware_size, operation)
      && UpgradePackage_CheckPlaintext(read, read_context, package_size, aes_key,
                                        firmware_size, write, write_context, operation);
}

// 检查向量地址是否落在有效 SRAM 和当前固件范围，不访问硬件。
bool UpgradePackage_CheckVector(const uint8_t *vector, uint32_t firmware_size)
{
  if ((vector == NULL) || (firmware_size < 8U) || (firmware_size > UPGRADE_PACKAGE_APP_CAPACITY))
  {
    return false;
  }
  uint32_t sp = ReadLe32(vector);
  uint32_t reset = ReadLe32(vector + 4U);
  uint32_t entry = reset & ~1U;
  return (sp > 0x20000040U) && (sp <= 0x20020000U) && ((sp & 7U) == 0U)
      && ((reset & 1U) != 0U) && (entry >= UPGRADE_PACKAGE_APP_ADDRESS + 8U)
      && (entry - UPGRADE_PACKAGE_APP_ADDRESS < firmware_size);
}

// 从解密输出中收集固件开头的向量字节。
static bool CaptureVector(void *context, size_t offset, const uint8_t *data, size_t size)
{
  if (offset < 8U)
  {
    size_t count = 8U - offset;
    if (count > size)
    {
      count = size;
    }
    memcpy((uint8_t *)context + offset, data, count);
  }
  return true;
}

// 完整认证后检查固件向量，输出可信镜像信息。
bool UpgradePackage_VerifyImage(UpgradePackageOperation *operation,
                                UpgradePackageRead read, void *context, size_t package_size,
                                const uint8_t *public_key, const uint8_t *aes_key,
                                UpgradePackageImage *image)
{
  if (image == NULL)
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_INVALID);
  }
  memset(image, 0, sizeof(*image));
  uint8_t manifest[UPGRADE_MANIFEST_SIZE];
  uint8_t vector[8] = {0};
  uint32_t firmware_size = 0U;
  uint16_t capabilities = 0U;
  if (!ReadPackage(operation, read, context, 0U, manifest, sizeof(manifest))
      || !UpgradePackage_CheckManifest(manifest, sizeof(manifest), package_size,
                                        &firmware_size, &capabilities))
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_INVALID);
  }
  if (!UpgradePackage_Decrypt(operation, read, context, package_size, public_key, aes_key,
                               CaptureVector, vector))
  {
    return false;
  }
  if (!UpgradePackage_CheckVector(vector, firmware_size))
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_IMAGE);
  }
  if (!CheckOperation(operation, false))
  {
    return false;
  }
  image->firmware_size = firmware_size;
  image->capabilities = capabilities;
  memcpy(image->digest, manifest + 60U, sizeof(image->digest));
  return true;
}
