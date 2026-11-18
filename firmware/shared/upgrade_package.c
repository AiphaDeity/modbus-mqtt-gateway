/*
 * App 接收认证与 Boot 安装共用同一套包检查，按块处理，不在 RAM 中保存整包。
 * 加密包布局为 128 B 包头、64 B 签名、固件密文和 16 B GCM 标签。
 * 签名认证来源，GCM 检查认证解密结果，明文摘要及向量检查目标镜像内容。
 */
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

/*
 * 使用可信 P-256 公钥核验已计算好的 32 B 摘要，签名输入为固定 64 B 的大端 r+s。
 * 不再次对 digest 做 SHA-256；同一上下文分段执行，所有出口释放对象并返回原库结果。
 */
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
  // 包内 r/s 与制包工具的定长大端编码对应，不能把这 64 B 当成 DER 签名解析。
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

/*
 * 从实际源包读取 M 和 S，检查布局后重算 H_sign=SHA256(M+C+T)，用可信公钥核验 S。
 * 签名区 [128,192) 必须跳过，区别于接收层包含 S 的整包传输摘要 H_package。
 * 本函数成功只证明签名检查通过，GCM、明文摘要和向量还由后续步骤核验。
 */
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

  // 1. 先读固定包头和紧随其后的 64 B 签名，格式检查不赋予包内字段可信性。
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
  // 2. 先散列整个 M，再从偏移 128+64 读到包尾，连续加入 C 和 T，不把 S 算进签名摘要。
  int result = mbedtls_sha256_starts(&hash, 0);
  if (result == 0)
  {
    result = mbedtls_sha256_update(&hash, manifest, sizeof(manifest));
  }

  // 源包仍按块读取，256 B 工作缓冲循环复用，不拼出完整签名消息到 RAM。
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
  // 3. 将刚重算的摘要与原始 r/s 交给分段验签；使用已置备公钥，不接受包内自带替代公钥。
  bool verified = UpgradePackage_CheckDigest(digest, signature, public_key, operation) == 0;
  if (!CheckOperation(operation, false) || !verified
      || !CheckOperation(operation, true))
  {
    return Fail(operation, UPGRADE_PACKAGE_ERROR_AUTH);
  }
  *firmware_size = checked_size;
  return true;
}

// 检查包头并核验 H_sign 对应的签名，输出固件长度；不代替 GCM、明文摘要和向量检查。
bool UpgradePackage_VerifySignature(UpgradePackageOperation *operation,
                                    UpgradePackageRead read, void *context, size_t package_size,
                                    const uint8_t *public_key, uint32_t *firmware_size)
{
  return UpgradePackage_CheckSignature(read, context, package_size, public_key,
                                       firmware_size, operation);
}

/*
 * 解密工作区按调用分配，避免 GCM 上下文和双缓冲叠加占用 Boot 启动栈。
 * 不保存完整固件，明文按块用于摘要计算或同步输出，返回前清零并释放整个工作区。
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

/*
 * 再次分块读取已验签的包，按包头 nonce 和完整 M/AAD 进行 AES-GCM 解密，并累计明文 H_image。
 * GCM 标签、长度和明文摘要均正确才返回成功；有 write 时属于上层完整认证后的同步输出遍。
 */
static bool UpgradePackage_CheckPlaintext(UpgradePackageRead read, void *context,
                                       size_t package_size, const uint8_t *aes_key,
                                       uint32_t firmware_size, UpgradePackageWrite write,
                                       void *write_context, UpgradePackageOperation *operation)
{
  if (!CheckOperation(operation, false))
  {
    return false;
  }
  // 只分配固定大小的密码工作区；失败立即返回，不缩小缓冲或跳过认证。
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

  // 原始 M 作为 AAD，nonce 位于偏移 48，明文摘要位于偏移 60；M 保持明文但参与 GCM 认证。
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

  // 只把 C 解密成临时明文块并更新 H_image，末尾 T 已单独取出，不能当作固件内容解密。
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
    // 有写回调时属于完整认证后的再次解密；调用方仍须等待本遍最终检查通过才放行镜像。
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
  // 先比较计算出的 GCM 标签 T，再比较 H_image；T 是对称认证标签，不能拿 ECDSA 签名 S 代替。
  // 两项使用常量时间比较，避免按第一个不同字节提前结束。
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
  // 所有成功和失败出口共用清理路径，明文及密码状态清零后再归还 C 堆。
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

/*
 * 先完整认证源包，再读同一包逐块解密并同步输出明文；安装和向量检查复用本流程。
 * 第一遍只核验签名、GCM 和明文摘要，不调用 write；第二遍输出后仍须通过本遍最终检查。
 * 第二遍失败时可能已有部分明文交出，调用方不得把已执行回调视为完整镜像认证成功。
 */
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
  // 短路求值保证第一遍认证通过才进入带 write 的第二遍；两遍共用 operation 的原固定期限。
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
  // 复位向量最低位是 Thumb 标记；比较地址时清除，实际跳转时保留。
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

/*
 * App 与 Boot 共用的只读镜像认证：完整包认证后收集前 8 B 向量，全部通过才输出可信 image。
 * 解密产生的明文仅在临时缓冲中处理，CaptureVector 只收集 MSP/入口，不写片内 App 或持久状态。
 */
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
  // 输出的是已经核验的明文镜像身份 H_image；VERIFIED/READY 等状态由各调用方随后决定。
  image->firmware_size = firmware_size;
  image->capabilities = capabilities;
  memcpy(image->digest, manifest + 60U, sizeof(image->digest));
  return true;
}
