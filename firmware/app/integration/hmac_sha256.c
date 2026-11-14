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

/* ===== SHA-256 核心算法实现 (FIPS 180-4) =====
 * SHA-256 将任意长度输入压缩为 256 位(32 字节)定长摘要。
 * 算法分三步：
 *   1. 补位 (padding)：输入末尾加 1 bit + 填 0 + 8 字节长度，凑成 64 字节整数倍
 *   2. 分块：每 64 字节(512 bit)为一块，逐块送入压缩函数 sha256_transform
 *   3. 压缩：每块经过 64 轮混合运算，更新 8 个 32 位工作变量(state)
 * 最终 state 的 8 个 32 位值拼接即 256 位摘要。 */

/* K[64]：SHA-256 轮常量表，来自前 64 个素数立方根的小数部分前 32 位。
 * 每轮压缩用 K[i] 做加法混合，保证算法的非线性与雪崩效应。
 * 标准固定值，不可修改（改了就不是标准 SHA-256，签名会对不上）。 */
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

/* ===== 位运算宏（SHA-256 标准定义）===== */
/* ROTR：32 位循环右移 n 位。右端移出的位从左端补回。 */
#define ROTR(x,n)  (((x) >> (n)) | ((x) << (32 - (n))))
/* CH(选择函数)：e 为 1 的位取 f，e 为 0 的位取 g。 */
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
/* MAJ(多数函数)：三个输入中多数为 1 则输出 1。 */
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
/* EP0/EP1(大 Sigma)：工作变量的扩散函数，循环移位+异或混合。 */
#define EP0(x)  (ROTR(x,2)  ^ ROTR(x,13) ^ ROTR(x,22))
#define EP1(x)  (ROTR(x,6)  ^ ROTR(x,11) ^ ROTR(x,25))
/* SIG0/SIG1(小 Sigma)：消息调度的扩散函数，用于扩展 W[16..63]。 */
#define SIG0(x) (ROTR(x,7)  ^ ROTR(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))

/* SHA-256 上下文：保存哈希计算的中间状态 */
typedef struct {
    uint32_t state[8];  /* 8 个 32 位工作变量 a~h，初始值为标准 IV，每块更新 */
    uint64_t bitlen;    /* 已处理的总比特数（用于最终补位时的长度字段） */
    uint8_t  data[64];  /* 当前块的缓冲区（凑满 64 字节才送入 transform） */
    size_t   datalen;   /* data 缓冲区当前已填充字节数（0~63） */
} sha256_ctx;

/* 初始化：设置标准初始向量 IV。
 * 8 个初始值来自前 8 个素数(2,3,5,7,11,13,17,19)平方根的小数部分前 32 位。
 * 这是 SHA-256 标准固定值，不可修改。 */
static void sha256_init(sha256_ctx *c)
{
    c->datalen = 0;
    c->bitlen  = 0;
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
}

/* 压缩函数（SHA-256 核心）：处理一个 64 字节(512 bit)块，更新 state。
 *
 * 算法分两阶段：
 *   1. 消息调度：把 16 个原始字 W[0..15] 扩展为 64 个字 W[0..63]
 *   2. 64 轮压缩：每轮用 W[i] + K[i] 混入工作变量 a~h
 *
 * 输入 data 必须恰好 64 字节。 */
static void sha256_transform(sha256_ctx *c, const uint8_t *data)
{
    uint32_t a, b, cc, d, e, f, g, h, t1, t2, m[64];
    int i, j;

    /* --- 阶段 1：消息调度 W[0..63] ---
     * W[0..15]：直接从输入块按大端读取 16 个 32 位字 */
    for (i = 0, j = 0; i < 16; i++, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j+1] << 16) |
               ((uint32_t)data[j+2] << 8) | ((uint32_t)data[j+3]);
    /* W[16..63]：用前序字递推扩展，SIG0/SIG1 提供非线性扩散 */
    for (; i < 64; i++)
        m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];

    /* --- 阶段 2：64 轮压缩 ---
     * 初始化工作变量为当前 state（a~h 对应 state[0..7]） */
    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];

    /* 64 轮循环，每轮：
     *   t1 = h + EP1(e) + CH(e,f,g) + K[i] + W[i]   ← 主混合（含密钥/消息注入）
     *   t2 = EP0(a) + MAJ(a,b,c)                     ← 辅助混合
     *   然后整体右移：h←g, g←f, f←e, e←d+t1, d←c, c←b, b←a, a←t1+t2 */
    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, cc);
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }

    /* 将 64 轮后的工作变量累加回 state（这就是本块的哈希更新） */
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

/* 喂入数据：逐字节追加到哈希计算。
 * 凑满 64 字节就调 transform 压缩一块，然后清空缓冲继续接收。
 * 可多次调用（流式），等全部数据喂完后调 sha256_final 取结果。 */
static void sha256_update(sha256_ctx *c, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        c->data[c->datalen++] = data[i];   /* 追加一个字节到缓冲 */
        if (c->datalen == 64) {            /* 缓冲满 64 字节 = 一个完整块 */
            sha256_transform(c, c->data);  /* 压缩这块 */
            c->bitlen += 512;              /* 已处理位数 +512 */
            c->datalen = 0;                /* 清空缓冲等下一块 */
        }
    }
}

/* 最终化：补位 + 输出 32 字节摘要。
 *
 * SHA-256 补位规则（FIPS 180-4）：
 *   1. 末尾追加 1 个 1 bit（即字节 0x80）
 *   2. 追加 0 bit 直到长度 ≡ 56 (mod 64)（即距 64 字节边界剩 8 字节）
 *   3. 最后 8 字节填入原始消息的总比特数（大端 64 位）
 * 补位后总长度是 64 字节的整数倍，可以完整送入 transform。
 *
 * out：32 字节摘要，大端序输出。 */
static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint32_t i = (uint32_t)c->datalen;

    /* 步骤 1：追加 0x80（1 bit + 7 个 0 bit） */
    c->data[i++] = 0x80;
    /* 如果剩余空间不够放 8 字节长度（i > 56），先填满当前块并压缩，再开新块 */
    if (i > 56) {
        while (i < 64) c->data[i++] = 0;
        sha256_transform(c, c->data);
        i = 0;
    }
    /* 步骤 2：填 0 直到第 56 字节（留最后 8 字节给长度） */
    while (i < 56) c->data[i++] = 0;

    /* 步骤 3：最后 8 字节 = 原始消息总比特数（大端 64 位） */
    c->bitlen += (uint64_t)c->datalen * 8;
    c->data[63] = (uint8_t)(c->bitlen);
    c->data[62] = (uint8_t)(c->bitlen >> 8);
    c->data[61] = (uint8_t)(c->bitlen >> 16);
    c->data[60] = (uint8_t)(c->bitlen >> 24);
    c->data[59] = (uint8_t)(c->bitlen >> 32);
    c->data[58] = (uint8_t)(c->bitlen >> 40);
    c->data[57] = (uint8_t)(c->bitlen >> 48);
    c->data[56] = (uint8_t)(c->bitlen >> 56);
    sha256_transform(c, c->data);   /* 压缩最后一块（含补位+长度） */

    /* 输出：8 个 32 位 state 值按大端转 32 字节 */
    for (i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->state[i] >> 24);
        out[i*4+1] = (uint8_t)(c->state[i] >> 16);
        out[i*4+2] = (uint8_t)(c->state[i] >> 8);
        out[i*4+3] = (uint8_t)(c->state[i]);
    }
}

/* ===== HMAC-SHA256 包装 (FIPS 198-1) =====
 * HMAC 用密钥对消息做两次哈希，实现带密钥的消息认证码。
 *
 * 算法：HMAC(K, m) = H( (K ⊕ opad) || H( (K ⊕ ipad) || m ) )
 *   K   = 密钥（超 64 字节先 hash，不足 64 字节补 0）
 *   ipad= 0x36 重复 64 次，opad = 0x5c 重复 64 次
 *   ⊕   = 异或
 *   H   = SHA-256
 * 内层哈希把消息混入，外层哈希把密钥再混一次，防止长度扩展攻击。
 *
 * 用途：阿里云/OneNET 等云平台的 MQTT 签名认证。 */
void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len,
                 uint8_t out[SHA256_DIGEST_SIZE])
{
    uint8_t k[64] = {0};       /* 规整后的密钥（补 0 到 64 字节） */
    uint8_t ipad[64], opad[64]; /* 内/外层填充 */
    uint8_t inner[32];         /* 内层哈希结果 */
    sha256_ctx c;
    int i;

    /* 密钥规整：超 64 字节先 hash 压缩到 32 字节再补 0；不足 64 直接补 0 */
    if (key_len > 64) {
        sha256_init(&c);
        sha256_update(&c, key, key_len);
        sha256_final(&c, k);
    } else {
        memcpy(k, key, key_len);
    }

    /* 生成内/外层填充：K 分别异或 ipad(0x36) 和 opad(0x5c) */
    for (i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    /* 内层：inner = SHA256( K⊕ipad || msg ) */
    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, msg, msg_len);
    sha256_final(&c, inner);

    /* 外层：out = SHA256( K⊕opad || inner ) */
    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

/* HMAC-SHA256 输出转小写 hex 字符串（64 字符 + '\0'）。
 * 阿里云 IoT 平台要求签名为 hex 字符串形式。 */
void hmac_sha256_hex(const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len,
                     char out_hex[HMAC_SHA256_HEX_LEN + 1])
{
    uint8_t dig[SHA256_DIGEST_SIZE];
    static const char hx[] = "0123456789abcdef";
    int i;

    hmac_sha256(key, key_len, msg, msg_len, dig);
    /* 每字节拆成高 4 位 + 低 4 位，映射到 hex 字符 */
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        out_hex[i*2]   = hx[dig[i] >> 4];
        out_hex[i*2+1] = hx[dig[i] & 0x0f];
    }
    out_hex[HMAC_SHA256_HEX_LEN] = '\0';
}

/* 纯 SHA256（非 HMAC），输出小写 hex 字符串。
 * 百度云 IoT Core 签名专用：
 *   password = lowercase_hex( SHA256(deviceKey + "&" + timestamp + "&SHA256" + deviceSecret) )
 * 注意：百度云用的是纯 SHA256（无 HMAC），密钥直接拼在消息末尾。 */
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
