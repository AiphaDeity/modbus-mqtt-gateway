/*
 * sizeof_extract.c —— mbedTLS 结构体 sizeof 精确测量（用真实 TLS 配置）
 *
 * 方法：每个结构体声明一个 volatile char 数组，数组大小 = sizeof(结构体)。
 *      __attribute__((used)) 防止优化；nm -S 直接读出每个数组的大小即 sizeof。
 *
 * 编译：
 *   arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -Os \
 *     '-DMBEDTLS_CONFIG_FILE="https_tls_mbedtls_config.h"' \
 *     -Itools -Ifirmware/shared -Ifirmware/app/services/upgrade \
 *     -Ifirmware/bootloader/third_party/mbedtls/include \
 *     -Ifirmware/bootloader/third_party/mbedtls/library \
 *     -c tools/sizeof_extract.c -o build/sizeof_extract.o
 * 读值：
 *   arm-none-eabi-nm -S --size-sort build/sizeof_extract.o   (Size 列即 sizeof)
 */
#include "mbedtls/build_info.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/cipher.h"
#include "mbedtls/md.h"
#include "mbedtls/rsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/bignum.h"
#include "mbedtls/version.h"
#include "ssl_misc.h"

/* volatile char 数组：大小即 sizeof，nm -S 的 Size 列直接读出 */
#define SZ(name, type) \
    volatile char name[sizeof(type)] __attribute__((used, aligned(1)))

SZ(arr_ssl_context,       mbedtls_ssl_context);
SZ(arr_ssl_config,        mbedtls_ssl_config);
SZ(arr_handshake_params,  mbedtls_ssl_handshake_params);
SZ(arr_transform,         mbedtls_ssl_transform);
SZ(arr_ssl_session,       mbedtls_ssl_session);
SZ(arr_x509_crt,          mbedtls_x509_crt);
SZ(arr_x509_buf,          mbedtls_x509_buf);
SZ(arr_pk_context,        mbedtls_pk_context);
SZ(arr_rsa_context,       mbedtls_rsa_context);
SZ(arr_ecp_keypair,       mbedtls_ecp_keypair);
SZ(arr_ecp_group,         mbedtls_ecp_group);
SZ(arr_ecdh_context,      mbedtls_ecdh_context);
SZ(arr_ctr_drbg,          mbedtls_ctr_drbg_context);
SZ(arr_entropy,           mbedtls_entropy_context);
SZ(arr_entropy_source,    mbedtls_entropy_source_state);
SZ(arr_cipher_ctx,        mbedtls_cipher_context_t);
SZ(arr_md_ctx,            mbedtls_md_context_t);
SZ(arr_mpi,               mbedtls_mpi);

/* TLS 收发缓冲长度（宏表达式，用 char[N] 测 N）*/
volatile char arr_in_buffer_len[MBEDTLS_SSL_IN_BUFFER_LEN] __attribute__((used, aligned(1)));
volatile char arr_out_buffer_len[MBEDTLS_SSL_OUT_BUFFER_LEN] __attribute__((used, aligned(1)));

/* 缓冲长度宏：用 enum 把宏值编进枚举（nm 显示枚举常量值） */
typedef enum {
    ev_in_buffer_len      = MBEDTLS_SSL_IN_BUFFER_LEN,
    ev_out_buffer_len     = MBEDTLS_SSL_OUT_BUFFER_LEN,
    ev_entropy_max_sources= MBEDTLS_ENTROPY_MAX_SOURCES,
} extract_enums_t;
