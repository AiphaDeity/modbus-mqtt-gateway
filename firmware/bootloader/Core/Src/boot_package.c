#include "boot_package.h"

#include <string.h>
#include <stdlib.h>
#include "mbedtls/constant_time.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/gcm.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

// 当前 V1 测试包的固定布局，与 Python 打包工具保持一致。
#define BOOT_MANIFEST_SIZE 128U
#define BOOT_SIGNATURE_SIZE 64U
#define BOOT_PACKAGE_CHUNK_SIZE 256U
#define BOOT_PACKAGE_OVERHEAD 208U
#define BOOT_PACKAGE_APP_ADDRESS 0x08010000U
#define BOOT_PACKAGE_APP_CAPACITY (448U * 1024U)

// 测试硬件标识占满 16 字节，字符串末尾和剩余位置均为零。
static const uint8_t boot_test_hardware_id[16] = "F407-TEST";

// 从可读缓冲解码小端整数，避免未对齐访问。
static uint16_t Boot_ReadLe16(const uint8_t *data)
{
  return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

static uint32_t Boot_ReadLe32(const uint8_t *data)
{
  return (uint32_t)data[0]
       | ((uint32_t)data[1] << 8U)
       | ((uint32_t)data[2] << 16U)
       | ((uint32_t)data[3] << 24U);
}

// 先验证缓冲边界，再按协议偏移读取字段，最后输出可用的固件长度。
bool Boot_PackageCheckManifest(const uint8_t *manifest,
                              size_t manifest_size,
                              size_t package_size,
                              uint32_t *firmware_size)
{
  if (firmware_size == NULL)
  {
    return false;
  }
  *firmware_size = 0U;

  if ((manifest == NULL) || (manifest_size != BOOT_MANIFEST_SIZE)
      || (package_size < BOOT_PACKAGE_OVERHEAD))
  {
    return false;
  }

  if ((memcmp(manifest, "GWUPD001", 8U) != 0)
      || (Boot_ReadLe16(&manifest[8]) != 1U)
      || (Boot_ReadLe16(&manifest[10]) != BOOT_MANIFEST_SIZE)
      || (memcmp(&manifest[12], boot_test_hardware_id, sizeof(boot_test_hardware_id)) != 0)
      || (Boot_ReadLe32(&manifest[32]) != BOOT_PACKAGE_APP_ADDRESS))
  {
    return false;
  }

  uint32_t plaintext_size = Boot_ReadLe32(&manifest[36]);
  uint32_t ciphertext_size = Boot_ReadLe32(&manifest[40]);
  // 已检查总长下限，用减法比较，避免包头中的不可信长度导致加法溢出。
  if ((plaintext_size == 0U) || (plaintext_size > BOOT_PACKAGE_APP_CAPACITY)
      || (ciphertext_size != plaintext_size)
      || (ciphertext_size != package_size - BOOT_PACKAGE_OVERHEAD))
  {
    return false;
  }

  // 当前测试版只识别编号 1，正式版本按批准的密钥映射和硬件标识调整。
  if ((Boot_ReadLe32(&manifest[44]) != 1U)
      || (Boot_ReadLe32(&manifest[108]) != 1U)
      || (Boot_ReadLe16(&manifest[112]) != 1U))
  {
    return false;
  }
  for (size_t i = 114U; i < BOOT_MANIFEST_SIZE; i++)
  {
    if (manifest[i] != 0U)
    {
      return false;
    }
  }

  *firmware_size = plaintext_size;
  return true;
}

// Mbed TLS 对象统一初始化和释放，任何导入或验签错误均返回 false。
bool Boot_PackageVerifyDigest(const uint8_t *digest,
                             const uint8_t *signature,
                             const uint8_t *public_key)
{
  if ((digest == NULL) || (signature == NULL) || (public_key == NULL)
      || (public_key[0] != 0x04U))
  {
    return false;
  }
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
    result = mbedtls_ecdsa_verify(&group, digest, 32U, &public_key_point, &r, &s);
  }

  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_ecp_point_free(&public_key_point);
  mbedtls_ecp_group_free(&group);
  return result == 0;
}

// 包头先做边界检查，再跳过签名区域，连续散列密文和标签。
bool Boot_PackageVerifySignature(Boot_PackageRead read, void *context,
                                 size_t package_size, const uint8_t *public_key,
                                 uint32_t *firmware_size)
{
  if (firmware_size == NULL)
  {
    return false;
  }
  *firmware_size = 0U;
  if ((read == NULL) || (public_key == NULL) || (public_key[0] != 0x04U)
      || (package_size <= BOOT_PACKAGE_OVERHEAD)
      || (package_size - BOOT_PACKAGE_OVERHEAD > BOOT_PACKAGE_APP_CAPACITY))
  {
    return false;
  }

  uint8_t manifest[BOOT_MANIFEST_SIZE];
  uint8_t signature[BOOT_SIGNATURE_SIZE];
  uint32_t checked_size = 0U;
  if (!read(context, 0U, manifest, sizeof(manifest))
      || !Boot_PackageCheckManifest(manifest, sizeof(manifest), package_size, &checked_size)
      || !read(context, BOOT_MANIFEST_SIZE, signature, sizeof(signature)))
  {
    return false;
  }

  uint8_t buffer[BOOT_PACKAGE_CHUNK_SIZE];
  uint8_t digest[32];
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  int result = mbedtls_sha256_starts(&hash, 0);
  if (result == 0)
  {
    result = mbedtls_sha256_update(&hash, manifest, sizeof(manifest));
  }

  size_t offset = BOOT_MANIFEST_SIZE + BOOT_SIGNATURE_SIZE;
  bool read_ok = true;
  while ((result == 0) && (offset < package_size))
  {
    size_t size = package_size - offset;
    if (size > sizeof(buffer))
    {
      size = sizeof(buffer);
    }
    if (!read(context, offset, buffer, size))
    {
      read_ok = false;
      break;
    }
    result = mbedtls_sha256_update(&hash, buffer, size);
    offset += size;
  }
  if ((result == 0) && read_ok)
  {
    result = mbedtls_sha256_finish(&hash, digest);
  }
  mbedtls_sha256_free(&hash);

  if ((result != 0) || !read_ok
      || !Boot_PackageVerifyDigest(digest, signature, public_key))
  {
    return false;
  }
  *firmware_size = checked_size;
  return true;
}


/*
 * 解密工作区按调用分配，避免 GCM 上下文和双缓冲叠加占用 Boot 启动栈。
 * 不保存完整固件，明文只用于摘要计算，返回前清零并释放整个工作区。
 */
typedef struct
{
  mbedtls_gcm_context gcm;
  mbedtls_sha256_context hash;
  uint8_t manifest[BOOT_MANIFEST_SIZE];
  uint8_t ciphertext[BOOT_PACKAGE_CHUNK_SIZE];
  uint8_t plaintext[BOOT_PACKAGE_CHUNK_SIZE + 15U];
  uint8_t expected_tag[16];
  uint8_t actual_tag[16];
  uint8_t digest[32];
} Boot_PackageDecryptState;

// 已验签的包再次按块读取，只在标签、长度和明文摘要全部正确时接受。
static bool Boot_PackageCheckPlaintext(Boot_PackageRead read, void *context,
                                       size_t package_size, const uint8_t *aes_key,
                                       uint32_t firmware_size)
{
  Boot_PackageDecryptState *state = calloc(1U, sizeof(*state));
  if (state == NULL)
  {
    return false;
  }
  mbedtls_gcm_init(&state->gcm);
  mbedtls_sha256_init(&state->hash);
  bool verified = false;
  uint32_t checked_size = 0U;
  if (!read(context, 0U, state->manifest, sizeof(state->manifest))
      || !Boot_PackageCheckManifest(state->manifest, sizeof(state->manifest),
                                    package_size, &checked_size)
      || (checked_size != firmware_size)
      || !read(context, package_size - sizeof(state->expected_tag),
               state->expected_tag, sizeof(state->expected_tag)))
  {
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
    goto cleanup;
  }

  size_t remaining = firmware_size;
  size_t offset = BOOT_MANIFEST_SIZE + BOOT_SIGNATURE_SIZE;
  size_t plaintext_size = 0U;
  while (remaining > 0U)
  {
    size_t size = remaining;
    if (size > sizeof(state->ciphertext))
    {
      size = sizeof(state->ciphertext);
    }
    size_t output_size = 0U;
    if (!read(context, offset, state->ciphertext, size)
        || (mbedtls_gcm_update(&state->gcm, state->ciphertext, size,
                                state->plaintext, sizeof(state->plaintext),
                                &output_size) != 0)
        || (output_size > firmware_size - plaintext_size)
        || (mbedtls_sha256_update(&state->hash, state->plaintext, output_size) != 0))
    {
      goto cleanup;
    }
    plaintext_size += output_size;
    mbedtls_platform_zeroize(state->plaintext, sizeof(state->plaintext));
    offset += size;
    remaining -= size;
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
    goto cleanup;
  }
  // 常量时间比较，避免按第一个不同字节提前结束比较。
  verified = (mbedtls_ct_memcmp(state->actual_tag, state->expected_tag, 16U) == 0)
          && (mbedtls_ct_memcmp(state->digest, &state->manifest[60], 32U) == 0);

cleanup:
  mbedtls_sha256_free(&state->hash);
  mbedtls_gcm_free(&state->gcm);
  mbedtls_platform_zeroize(state, sizeof(*state));
  free(state);
  return verified;
}

// 先认证包的来源和密文，再检查解密结果，不向外输出尚未认证的明文。
bool Boot_PackageVerify(Boot_PackageRead read,
                        void *context,
                        size_t package_size,
                        const uint8_t *public_key,
                        const uint8_t *aes_key,
                        uint32_t *firmware_size)
{
  if (firmware_size == NULL)
  {
    return false;
  }
  *firmware_size = 0U;
  uint32_t checked_size = 0U;
  if ((aes_key == NULL)
      || !Boot_PackageVerifySignature(read, context, package_size, public_key, &checked_size)
      || !Boot_PackageCheckPlaintext(read, context, package_size, aes_key, checked_size))
  {
    return false;
  }
  *firmware_size = checked_size;
  return true;
}
