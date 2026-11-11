/*
 * 轻量 HMAC-SHA256 实现（不依赖 mbedTLS）。
 *
 * 标准算法（FIPS 198-1 + FIPS 180-4），纯 C 实现，约 3KB Flash。
 * 块大小 64 字节，digest 32 字节。
 *
 * 用于云平台 MQTT 接入签名：
 *   百度云 IoT Core：password = lowercase_hex( SHA256(deviceKey&ts&SHA256deviceSecret) )
 *   阿里云/OneNET：HMAC-SHA256 变体
 */
#include "hmac_sha256.h"
#include <string.h>

/* ===== SHA-256 核心 ===== */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x,n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROTR(x,2)  ^ ROTR(x,13) ^ ROTR(x,22))
#define EP1(x)  (ROTR(x,6)  ^ ROTR(x,11) ^ ROTR(x,25))
#define SIG0(x) (ROTR(x,7)  ^ ROTR(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    size_t   datalen;
} sha256_ctx;

static void sha256_init(sha256_ctx *c)
{
    c->datalen = 0;
    c->bitlen  = 0;
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
}

static void sha256_transform(sha256_ctx *c, const uint8_t *data)
{
    uint32_t a, b, cc, d, e, f, g, h, t1, t2, m[64];
    int i, j;

    for (i = 0, j = 0; i < 16; i++, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j+1] << 16) |
               ((uint32_t)data[j+2] << 8) | ((uint32_t)data[j+3]);
    for (; i < 64; i++)
        m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];

    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, cc);
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha256_update(sha256_ctx *c, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        c->data[c->datalen++] = data[i];
        if (c->datalen == 64) {
            sha256_transform(c, c->data);
            c->bitlen += 512;
            c->datalen = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint32_t i = (uint32_t)c->datalen;

    /* 补位：1 后跟 0，直到 56(mod 64)，再补 8 字节长度 */
    c->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) c->data[i++] = 0;
        sha256_transform(c, c->data);
        i = 0;
    }
    while (i < 56) c->data[i++] = 0;

    c->bitlen += (uint64_t)c->datalen * 8;
    c->data[63] = (uint8_t)(c->bitlen);
    c->data[62] = (uint8_t)(c->bitlen >> 8);
    c->data[61] = (uint8_t)(c->bitlen >> 16);
    c->data[60] = (uint8_t)(c->bitlen >> 24);
    c->data[59] = (uint8_t)(c->bitlen >> 32);
    c->data[58] = (uint8_t)(c->bitlen >> 40);
    c->data[57] = (uint8_t)(c->bitlen >> 48);
    c->data[56] = (uint8_t)(c->bitlen >> 56);
    sha256_transform(c, c->data);

    /* 输出大端 */
    for (i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->state[i] >> 24);
        out[i*4+1] = (uint8_t)(c->state[i] >> 16);
        out[i*4+2] = (uint8_t)(c->state[i] >> 8);
        out[i*4+3] = (uint8_t)(c->state[i]);
    }
}

/* ===== HMAC-SHA256 包装 ===== */

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len,
                 uint8_t out[SHA256_DIGEST_SIZE])
{
    uint8_t k[64] = {0};
    uint8_t ipad[64], opad[64];
    uint8_t inner[32];
    sha256_ctx c;
    int i;

    /* key 超 64 字节先 hash；否则补 0 到 64 */
    if (key_len > 64) {
        sha256_init(&c);
        sha256_update(&c, key, key_len);
        sha256_final(&c, k);
    } else {
        memcpy(k, key, key_len);
    }

    for (i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    /* inner = H(k ^ ipad || msg) */
    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, msg, msg_len);
    sha256_final(&c, inner);

    /* out = H(k ^ opad || inner) */
    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

void hmac_sha256_hex(const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len,
                     char out_hex[HMAC_SHA256_HEX_LEN + 1])
{
    uint8_t dig[SHA256_DIGEST_SIZE];
    static const char hx[] = "0123456789abcdef";
    int i;

    hmac_sha256(key, key_len, msg, msg_len, dig);
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        out_hex[i*2]   = hx[dig[i] >> 4];
        out_hex[i*2+1] = hx[dig[i] & 0x0f];
    }
    out_hex[HMAC_SHA256_HEX_LEN] = '\0';
}

/* 纯 SHA256（非 HMAC），百度云 IoT Core 签名用 */
void sha256_hex(const uint8_t *msg, size_t msg_len,
                char out_hex[HMAC_SHA256_HEX_LEN + 1])
{
    uint8_t dig[SHA256_DIGEST_SIZE];
    sha256_ctx c;
    static const char hx[] = "0123456789abcdef";
    int i;

    sha256_init(&c);
    sha256_update(&c, msg, msg_len);
    sha256_final(&c, dig);

    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        out_hex[i*2]   = hx[dig[i] >> 4];
        out_hex[i*2+1] = hx[dig[i] & 0x0f];
    }
    out_hex[HMAC_SHA256_HEX_LEN] = '\0';
}
