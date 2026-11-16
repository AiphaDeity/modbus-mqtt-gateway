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
#include "hmac_sha256.h"           // 本文件对应的头文件，里面有 SHA256_DIGEST_SIZE 等宏定义
#include <string.h>                // C 标准字符串库，提供 memcpy 内存拷贝函数

/* ===== SHA-256 核心算法实现 (FIPS 180-4) =====
 * SHA-256 将任意长度输入压缩为 256 位(32 字节)定长摘要。
 * 算法分三步：
 *   1. 补位 (padding)：输入末尾加 1 bit + 填 0 + 8 字节长度，凑成 64 字节整数倍
 *   2. 分块：每 64 字节(512 bit)为一块，逐块送入压缩函数 sha256_transform
 *   3. 压缩：每块经过 64 轮混合运算，更新 8 个 32 位工作变量(state)
 * 最终 state 的 8 个 32 位值拼接即 256 位摘要。 */

// K[64]：SHA-256 轮常量表，来自前 64 个素数立方根的小数部分前 32 位
// static 表示只在本文件里能看到，const 表示只读不能改
// 每轮压缩用 K[i] 做加法混合，保证算法的非线性与雪崩效应
// 标准固定值，绝对不能修改，改了就不是标准 SHA-256，签名会对不上
static const uint32_t K[64] = {                // 64轮压缩运算用的标准常量表，算法规定不能改
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,  // 第 0-3 轮常量
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,  // 第 4-7 轮常量
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,  // 第 8-11 轮常量
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,  // 第 12-15 轮常量
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,  // 第 16-19 轮常量
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,  // 第 20-23 轮常量
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,  // 第 24-27 轮常量
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,  // 第 28-31 轮常量
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,  // 第 32-35 轮常量
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,  // 第 36-39 轮常量
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,  // 第 40-43 轮常量
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,  // 第 44-47 轮常量
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,  // 第 48-51 轮常量
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,  // 第 52-55 轮常量
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,  // 第 56-59 轮常量
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2   // 第 60-63 轮常量
};

// 位运算宏（SHA-256 标准定义），都是纯位操作，没有函数调用开销

// ROTR：32 位循环右移 n 位，右端移出的位从左端补回
// 比如 0x80000000 右移1位就变成 0x40000000，再循环右移1位就变成 0x00000001
#define ROTR(x,n)  (((x) >> (n)) | ((x) << (32 - (n))))   // 循环右移宏：右移出的位从左边补回来

// CH（选择函数）：x 为 1 的位取 y，x 为 0 的位取 z
// 就像一个开关，x=1 选 y，x=0 选 z
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))           // 选择函数宏：x的位是1取y，是0取z

// MAJ（多数函数）：三个输入中多数为 1 则输出 1，少数服从多数
// 三个里至少两个是1，结果就是1
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))  // 多数函数宏：三输入少数服从多数

// EP0/EP1（大 Sigma）：工作变量的扩散函数，循环移位+异或混合
// 把一个数的不同位打乱，让输入的变化能扩散到更多位
#define EP0(x)  (ROTR(x,2)  ^ ROTR(x,13) ^ ROTR(x,22))    // 大Sigma0宏：工作变量扩散
#define EP1(x)  (ROTR(x,6)  ^ ROTR(x,11) ^ ROTR(x,25))    // 大Sigma1宏：工作变量扩散

// SIG0/SIG1（小 Sigma）：消息调度的扩散函数，用于扩展 W[16..63]
// 把16个原始字扩展成64个，让消息的每一位都能影响后面的计算
#define SIG0(x) (ROTR(x,7)  ^ ROTR(x,18) ^ ((x) >> 3))    // 小sigma0宏：消息调度扩展用
#define SIG1(x) (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))   // 小sigma1宏：消息调度扩展用

// SHA-256 上下文：保存哈希计算的中间状态，整个计算过程中一直带着这个结构体
// typedef 给这个结构体起个名字叫 sha256_ctx，后面用起来方便
typedef struct {                               // SHA256计算中间状态，边喂数据边更新
    uint32_t state[8];   // 8 个 32 位工作变量 a~h，初始值为标准 IV，每处理一块就更新一次
    uint64_t bitlen;     // 已处理的总比特数，最终补位时要把这个长度填到最后8字节里
    uint8_t  data[64];   // 当前块的缓冲区，凑满 64 字节才送入压缩函数
    size_t   datalen;    // data 缓冲区当前已填充了多少字节，范围 0~63
} sha256_ctx;                                  // 结构体别名定义结束，后面用sha256_ctx当类型名

// 函数：sha256_init —— 初始化哈希上下文
// 功能：每次开始算SHA256前调用，把上下文重置成标准初始状态：8个状态字设为官方IV（前8个素数平方根小数部分前32位，固定值不能改），缓冲区清空，总长度清零
// 参数：c —— SHA256上下文结构体指针，保存中间状态，一次哈希从头用到尾
// 返回：void，无返回值
static void sha256_init(sha256_ctx *c)         // 初始化哈希上下文，设置标准初始值IV
{
    c->datalen = 0;                              // 缓冲区当前长度清零，还没填数据
    c->bitlen  = 0;                              // 已处理总比特数清零，还没处理数据
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;  // 前两个初始值
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;  // 中间两个初始值
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;  // 再两个初始值
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;  // 最后两个初始值
}

// 函数：sha256_transform —— SHA256核心压缩函数
// 功能：处理恰好64字节（512bit）的数据块，执行算法核心：先做64轮消息调度扩展W数组，再做64轮压缩循环更新8个状态字，这是SHA256运算量最集中的地方
// 参数：c —— 哈希上下文，状态字在压缩中被更新；data —— 指向待处理数据块的指针，必须恰好64字节
// 返回：void，无返回值，结果累积在上下文的状态字里
static void sha256_transform(sha256_ctx *c, const uint8_t *data)  // 压缩函数：处理一个64字节块
{
    // 定义8个工作变量 a~h、两个临时变量 t1/t2、消息调度数组 m[64]
    uint32_t a, b, cc, d, e, f, g, h, t1, t2, m[64];   // 8个工作变量+2个临时+64个消息调度字
    int i, j;                                      // 循环变量，i计数轮次，j定位字节

    // ===== 阶段 1：消息调度，把16个原始字扩展成64个 =====
    // W[0..15]：直接从输入块按大端读取16个32位字
    for (i = 0, j = 0; i < 16; i++, j += 4)      // 前16个字：每次读4字节，j按4递增
        // 把4个字节拼成一个32位字，大端序：高字节在前
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j+1] << 16) |
               ((uint32_t)data[j+2] << 8) | ((uint32_t)data[j+3]);

    // W[16..63]：用前面的字递推扩展，SIG0/SIG1 提供非线性扩散
    for (; i < 64; i++)                           // 后48个字：从i=16继续，递推扩展到64
        m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];  // 标准递推公式，前面的字算后面

    // ===== 阶段 2：64 轮压缩运算 =====
    // 先把当前 state 复制到工作变量 a~h，每轮更新这8个变量
    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];  // 复制state前4个到工作变量
    e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];    // 复制state后4个到工作变量

    // 64 轮循环，每轮做一次混合，然后8个变量整体右移
    for (i = 0; i < 64; i++) {                   // 64轮压缩循环开始
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];  // 主混合：消息和常量都从这里注入
        t2 = EP0(a) + MAJ(a, b, cc);                  // 辅助混合
        // 8个变量整体右移：h←g, g←f, f←e, e←d+t1, d←c, c←b, b←a, a←t1+t2
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }                                          // 64轮压缩循环结束

    // 64轮算完了，把工作变量累加回 state，这就是这一块对哈希的更新
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;   // 工作变量累加回state前4个
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;    // 工作变量累加回state后4个
}

// 函数：sha256_update —— 流式喂入数据
// 功能：分多次把任意长度的数据喂进哈希：数据先进64字节缓冲，凑满一块就调sha256_transform压缩一块并清缓冲继续收，同时累计已处理总字节数；全部喂完后调sha256_final取最终摘要
// 参数：c —— 哈希上下文指针，跨多次update保持状态；data —— 本次喂入数据起始指针；len —— 本次数据字节数，可以任意长度
// 返回：void，无返回值
static void sha256_update(sha256_ctx *c, const uint8_t *data, size_t len)  // 喂入数据，流式处理
{                                               // 函数体开始
    for (size_t i = 0; i < len; i++) {           // 逐字节遍历输入数据
        c->data[c->datalen++] = data[i];   // 追加一个字节到缓冲区，然后长度加1
        if (c->datalen == 64) {            // 缓冲满64字节，凑成一个完整块
            sha256_transform(c, c->data);  // 压缩处理这一块
            c->bitlen += 512;              // 已处理总比特数加512（64字节=512比特）
            c->datalen = 0;                // 清空缓冲区，准备接收下一块
        }
    }
}

// 函数：sha256_final —— 最终化并输出摘要
// 功能：按FIPS 180-4规范收尾：先补一个1bit再补0凑齐56字节，最后8字节填总位长度凑成64字节整数倍，把剩余数据压缩完，输出8个状态字即32字节最终摘要（大端序）
// 参数：c —— 哈希上下文指针，用完后可重新init复用；out —— 输出数组，至少32字节，接收最终哈希值
// 返回：void，无返回值，结果写进out
static void sha256_final(sha256_ctx *c, uint8_t out[32])   // 最终化：补位并输出32字节摘要
{
    uint32_t i = (uint32_t)c->datalen;           // 拿当前缓冲区已填充的长度

    // 步骤 1：追加 0x80（二进制 10000000，就是1个1bit加7个0bit）
    c->data[i++] = 0x80;                         // 步骤1：追加1个1bit（0x80=10000000），i同时后移
    // 如果剩余空间不够放8字节长度（i > 56），先填满当前块压缩，再开新块
    if (i > 56) {                                // 剩余空间不足8字节放长度
        while (i < 64) c->data[i++] = 0;         // 填0直到64字节
        sha256_transform(c, c->data);             // 压缩当前块
        i = 0;                                    // 新块从0开始
    }
    // 步骤 2：填 0 直到第 56 字节，留最后 8 字节给长度字段
    while (i < 56) c->data[i++] = 0;             // 步骤2：填0到第56字节，留8字节给长度

    // 步骤 3：最后 8 字节填原始消息的总比特数，大端 64 位
    c->bitlen += (uint64_t)c->datalen * 8;       // 加上最后缓冲区里的字节数，算总比特数
    c->data[63] = (uint8_t)(c->bitlen);          // 最低字节（第8字节）
    c->data[62] = (uint8_t)(c->bitlen >> 8);     // 右移8位，拿第7字节
    c->data[61] = (uint8_t)(c->bitlen >> 16);    // 右移16位，拿第6字节
    c->data[60] = (uint8_t)(c->bitlen >> 24);    // 右移24位，拿第5字节
    c->data[59] = (uint8_t)(c->bitlen >> 32);    // 右移32位，拿第4字节
    c->data[58] = (uint8_t)(c->bitlen >> 40);    // 右移40位，拿第3字节
    c->data[57] = (uint8_t)(c->bitlen >> 48);    // 右移48位，拿第2字节
    c->data[56] = (uint8_t)(c->bitlen >> 56);    // 右移56位，拿最高字节（第1字节）
    sha256_transform(c, c->data);                 // 压缩最后一块（含补位+长度）

    // 输出：8 个 32 位 state 值按大端序转成 32 字节
    for (i = 0; i < 8; i++) {                    // 8个state值按大端序转成32字节输出
        out[i*4]   = (uint8_t)(c->state[i] >> 24);  // 最高字节
        out[i*4+1] = (uint8_t)(c->state[i] >> 16);  // 次高字节
        out[i*4+2] = (uint8_t)(c->state[i] >> 8);   // 次低字节
        out[i*4+3] = (uint8_t)(c->state[i]);        // 最低字节
    }                                           // 输出循环结束
}

// 函数：hmac_sha256 —— 计算HMAC-SHA256（对外接口）
// 功能：带密钥的HMAC算法：密钥先规范到64字节，内层哈希把密钥异或0x36后拼消息算一次，外层哈希把密钥异或0x5c再拼内层结果算一次；双层结构防止长度扩展攻击，云平台MQTT签名认证用
// 参数：key —— 密钥起始指针；key_len —— 密钥字节数（超64会先被哈希压缩）；msg —— 消息起始指针；msg_len —— 消息字节数；out —— 输出数组，至少32字节接收HMAC结果
// 返回：void，无返回值，结果写进out
void hmac_sha256(const uint8_t *key, size_t key_len,       // 对外接口：算带密钥的HMAC
                 const uint8_t *msg, size_t msg_len,
                 uint8_t out[SHA256_DIGEST_SIZE])
{
    uint8_t k[64] = {0};       // 规整后的密钥，补0到64字节
    uint8_t ipad[64], opad[64]; // 内层填充和外层填充
    uint8_t inner[32];         // 内层哈希结果，32字节
    sha256_ctx c;               // SHA256 上下文
    int i;                      // 循环变量

    // 密钥规整：超64字节先哈希压缩到32字节再补0；不足64字节直接补0
    if (key_len > 64) {                          // 密钥超64字节，先哈希压缩
        sha256_init(&c);                          // 初始化SHA256
        sha256_update(&c, key, key_len);          // 喂入密钥
        sha256_final(&c, k);                      // 算出哈希，放到k的前32字节，后面自动补0
    } else {                                     // 密钥不超过64字节
        memcpy(k, key, key_len);                  // 直接把密钥拷到k里，后面自动补0
    }                                            // 密钥规整结束

    // 生成内/外层填充：密钥分别异或 ipad(0x36) 和 opad(0x5c)
    for (i = 0; i < 64; i++) {                   // 逐字节生成内外层填充
        ipad[i] = k[i] ^ 0x36;                    // 内层填充：密钥异或0x36
        opad[i] = k[i] ^ 0x5c;                    // 外层填充：密钥异或0x5c
    }                                            // 填充生成结束

    // 内层哈希：inner = SHA256( K⊕ipad || 消息 )
    sha256_init(&c);                              // 初始化
    sha256_update(&c, ipad, 64);                  // 先喂入内层填充
    sha256_update(&c, msg, msg_len);              // 再喂入消息
    sha256_final(&c, inner);                      // 算出内层哈希结果

    // 外层哈希：out = SHA256( K⊕opad || 内层结果 )
    sha256_init(&c);                              // 重新初始化
    sha256_update(&c, opad, 64);                  // 先喂入外层填充
    sha256_update(&c, inner, 32);                 // 再喂入内层哈希结果
    sha256_final(&c, out);                        // 算出最终的HMAC结果
}

// 函数：hmac_sha256_hex —— HMAC结果转hex字符串（对外接口）
// 功能：调hmac_sha256算出32字节二进制结果后逐字节转成64个小写hex字符加'\0'结尾，云平台签名一般要求提交字符串形式而不是原始二进制
// 参数：key —— 密钥指针；key_len —— 密钥长度；msg —— 消息指针；msg_len —— 消息长度；out_hex —— 输出字符数组，容量需HMAC_SHA256_HEX_LEN+1=65字节（64字符+结束符）
// 返回：void，无返回值，hex字符串写进out_hex
void hmac_sha256_hex(const uint8_t *key, size_t key_len,   // 对外接口：HMAC结果转hex字符串
                     const uint8_t *msg, size_t msg_len,
                     char out_hex[HMAC_SHA256_HEX_LEN + 1])
{
    uint8_t dig[SHA256_DIGEST_SIZE];              // 存二进制的HMAC结果
    static const char hx[] = "0123456789abcdef";  // hex字符表，0-9加a-f，共16个
    int i;                                       // 循环变量

    hmac_sha256(key, key_len, msg, msg_len, dig);  // 先算出二进制的HMAC结果

    // 每个字节拆成高4位和低4位，分别映射到hex字符
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {   // 32字节逐个转两个hex字符
        out_hex[i*2]   = hx[dig[i] >> 4];         // 高4位右移4位，查字符表
        out_hex[i*2+1] = hx[dig[i] & 0x0f];       // 低4位和0x0f按位与，查字符表
    }                                            // 转换循环结束
    out_hex[HMAC_SHA256_HEX_LEN] = '\0';          // 最后加字符串结束符
}

// 函数：sha256_hex —— 纯SHA256转hex字符串（对外接口）
// 功能：百度云IoT Core专用签名：password = 小写hex(SHA256(设备Key+"&"+时间戳+"&SHA256"+设备密钥))；注意百度云是纯SHA256不带HMAC，密钥直接拼在消息串末尾参与哈希
// 参数：msg —— 待哈希消息起始指针；msg_len —— 消息字节数；out_hex —— 输出字符数组，容量需65字节（64个hex字符+结束符）
// 返回：void，无返回值，hex结果写进out_hex
void sha256_hex(const uint8_t *msg, size_t msg_len,       // 对外接口：纯SHA256转hex字符串，百度云签名用
                char out_hex[HMAC_SHA256_HEX_LEN + 1])
{
    uint8_t dig[SHA256_DIGEST_SIZE];              // 存二进制的SHA256结果
    sha256_ctx c;                                  // SHA256上下文
    static const char hx[] = "0123456789abcdef";  // hex字符表
    int i;                                       // 循环变量

    sha256_init(&c);                              // 初始化SHA256
    sha256_update(&c, msg, msg_len);              // 喂入消息
    sha256_final(&c, dig);                        // 算出二进制哈希结果

    // 每个字节拆成高4位和低4位，转成hex字符
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {   // 32字节逐个转两个hex字符
        out_hex[i*2]   = hx[dig[i] >> 4];         // 高4位
        out_hex[i*2+1] = hx[dig[i] & 0x0f];       // 低4位
    }                                            // 转换循环结束
    out_hex[HMAC_SHA256_HEX_LEN] = '\0';          // 加字符串结束符
}
