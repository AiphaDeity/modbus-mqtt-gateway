# HTTPS 固件下载 MCU 资源预算报告（v4，真实链接 + 修正峰值）

> 生成时间：2026-11-12 15:50
> 基线 commit：`ddee3f1`（main，PR !8 收紧整机内存）
> 构建预设：AppDebug（APP.ld, offset=0x00010000, FLASH 448KiB）
> mbedTLS：3.6.7
> TLS 库：**已实际编译并链接进 ELF**（26 源 + stubs，统一配置）
> sizeof：ARM 实测（统一配置，`nm -S` 读出，`build/sizeof_extract.o` 可复核）

---

## 0. v4 相对 v3 的修正（审查第二轮反馈）

| 问题 | v3 错误 | v4 修正 |
|------|---------|---------|
| 库编译失败 | 缺 MBEDTLS_NO_PLATFORM_ENTROPY，entropy_poll.c 报错；硬件熵源/时间未接入 | 加 NO_PLATFORM_ENTROPY + PLATFORM_TIME_ALT/MS_TIME_ALT + stubs（ms_time/platform_time/hardware_poll/_gettimeofday）|
| 宏无效 | MBEDTLS_SSL_DEFAULT_CIPHERSUITES（3.6.7 无此宏） | 删除；靠只启用 KEY_EXCHANGE_ECDHE_RSA 自然限制套件 |
| gcm 配置不统一 | TLS 未启用 BLOCK_CIPHER_C，gcm_context=384；升级=616，混链错配 | 显式启用 BLOCK_CIPHER_C，**gcm_context 统一 616B**；所有源共用 https_tls_mbedtls_config.h |
| transform 重复 | 算 transform×2 | ssl_setup 只 calloc **1 个 transform_negotiate**(168B)；含 cipher_ctx_enc/dec |
| GCM 动态没算全 | 漏 gcm_context 动态分配 | 每个 transform 的 enc/dec cipher setup calloc gcm_context(616) → **×2=1232B** |
| 证书 DER 累加 | 把 4644B DER 当额外副本 | 握手走 `x509_crt_parse_der_nocopy`(ssl_tls.c:7910)，DER 引用 in_buf，**不额外占** |
| Flash 增量估算 | ~120KiB 估算 | **实际链接实测 77.2KiB** |
| 时间配置缺失 | 证书有效期无法检查 | 加 HAVE_TIME/HAVE_TIME_DATE |

---

## 1. 基线 + 含 TLS 真实链接结果

### 不含 TLS 基线（ddee3f1 AppDebug）

| 区域 | 占用 | 剩余 |
|------|------|------|
| RAM | 120104B | 10904B ≈ 10.65KiB |
| CCMRAM | 60828B | 4708B ≈ 4.60KiB |
| FLASH | 352128B | 106624B ≈ 104.12KiB |
| FreeRTOS heap | 24576B | 24KiB（RTOS 占用需运行期测） |
| C 堆 sbrk 跨度 | — | 11416B ≈ 11.15KiB（审查者口径 10.42） |

### 含 TLS 真实链接（26 源 + stubs + anchor，统一配置，链接通过 exit 0）

| 区域 | 占用 | 剩余 | 增量 |
|------|------|------|------|
| RAM | 120120B | 10888B ≈ 10.63KiB | +16B（静态几乎不增） |
| CCMRAM | 60828B | 4708B ≈ 4.60KiB | +0 |
| **FLASH** | **431132B** | **27620B ≈ 26.97KiB** | **+79004B ≈ 77.2KiB** |

> **Flash 实测增量 77.2KiB**（v2 估算 120KiB，实测少 43KiB）。含 TLS 后 Flash 剩 **26.97KiB，不缺**。
> TLS 符号 124 个已确认链接进 ELF（ssl_setup/handshake/x509_crt_verify/pk_parse/ctr_drbg 等）。

---

## 2. ARM 实测 sizeof（统一配置，BLOCK_CIPHER_C 启用，gcm=616）

| 结构体 | 实测 sizeof(B) | 分配 | 说明 |
|--------|------|------|------|
| mbedtls_ssl_context | 212 | 栈/static | |
| mbedtls_ssl_config | 108 | static[CCM] | |
| mbedtls_ssl_handshake_params | 572 | calloc | 含 transform 指针(非对象)、ecdh_context(240) |
| mbedtls_ssl_transform | 168 | calloc ×1 | **ssl_setup 只分 1 个**(transform_negotiate)；含 cipher_ctx_enc/dec |
| mbedtls_ssl_session | 120 | calloc ×1 | |
| mbedtls_x509_crt | 404 | 每张 | |
| mbedtls_pk_context | 8 | 每张内 | |
| mbedtls_rsa_context | 120 | pk_verify 内 | |
| mbedtls_gcm_context | **616** | **cipher setup 时 calloc** | 统一（与升级算法一致）|
| mbedtls_ctr_drbg_context | 320 | static[CCM] | |
| mbedtls_entropy_context | 60(MAX2)/420(MAX20) | static[CCM] | 已含 source[] |
| mbedtls_entropy_source_state | 20 | 嵌入 | 5字段×4B |
| mbedtls_mpi | 8 | 句柄(栈) | p 指向动态 limb |
| **MBEDTLS_SSL_IN_BUFFER_LEN** | 16429 | calloc | =45+16384 |
| **MBEDTLS_SSL_OUT_BUFFER_LEN** | 16429 / 2093(候选2) | calloc | |

> entropy(MAX20)=420B **已含 source[20]×20=400B**（v2 重复计数已修正）。
> 产物：`build/sizeof_extract.o`（`arm-none-eabi-nm -S --size-sort` 复核）。

---

## 3. 按分配生命周期算握手峰值（修正后）

### ssl_setup 动态分配（握手开始即存在）
- in_buf(16429) + out_buf(16429/2093)
- handshake_params(572) ×1
- transform_negotiate(168) ×1  ← **只 1 个，非 2 个**
- session_negotiate(120) ×1

### cipher setup 动态（transform 的 enc/dec）
- gcm_context(616) ×2 = 1232  ← **v3 漏算，已补**

### 握手期间证书链（nocopy）
- x509_crt ×3 = 1212 + pk ×3 = 24 = 1236  ← **DER 不额外占（nocopy 引用 in_buf）**

### 峰值汇总

| 阶段 | 候选1(16K/16K) | 候选2(16K/2K) |
|------|---------------|---------------|
| **握手动态峰值** | 16429+16429+572+168+1232+120+1236 = **36186B ≈ 35.3KiB** | 16429+2093+572+168+1232+120+1236 = **21850B ≈ 21.3KiB** |
| 下载阶段(不KEEP,handshake+证书释放) | 16429+16429+168+1232+120 = **34378B ≈ 33.6KiB** | 16429+2093+168+1232+120 = **20042B ≈ 19.6KiB** |

### CCM 静态增量
- ssl_config(108) + ctr_drbg(320) + entropy(60,MAX2) = **484B**（CCM 剩 4.60KiB，够）

---

## 4. 整机对比（含 TLS 基线）

| 分配器 | 可用 | 候选1握手(35.3KiB) | 候选2握手(21.3KiB) |
|--------|------|--------------------|--------------------|
| C 堆(sbrk) | ~11.15KiB | 不够 | 不够 |
| CCMRAM 剩 | 4.60KiB | 不够 | 不够 |
| FreeRTOS heap_4 | 池24KiB(RTOS占用需测) | 空池不够 | **空池 24>21.3，可能够** |
| **FLASH** | **剩26.97KiB** | — | **够** |

### 能确定
1. **Flash 够**：含 TLS 实测剩 26.97KiB（v2"缺58KiB"被推翻）
2. **C 堆放不下单次 in_buf**：16429B > sbrk 11.15KiB
3. **握手峰值超 C堆/CCM**：候选1 35.3 / 候选2 21.3，均 > 11.15(C堆) / 4.60(CCM)

### 不能确定（需运行期）
- **候选2走 heap_4 是否够**：握手 21.3KiB < 空池 24KiB，但 RTOS 对象占多少需 `xPortGetMinimumEverFreeHeapSize()` 实测
- **任务栈高水位**：当前 6KiB[CCM]，需 `uxTaskGetStackHighWaterMark`
- **真实握手记录**：BCE OTA URL 的 SNI/ECDHE 曲线/候选2可用性

---

## 5. M2 为验证做的工程改动（待集成时替换）

为完成实际链接验证，M2 新增/修改（均在 main ddee3f1 工作树，未提交）：
- `firmware/shared/https_tls_mbedtls_config.h`（统一 TLS 配置）
- `firmware/app/services/upgrade/https_tls_stubs.c`（ms_time/platform_time/hardware_poll/_gettimeofday stub；⚠️硬件熵源和 RTC 待接真实现）
- `firmware/app/services/upgrade/https_tls_link_anchor.c`（链接锚点，集成后删除）
- `firmware/app/CMakeLists.txt`（加 26 TLS 源 + stubs + anchor + --undefined 链接标志）

> 集成真实 HTTPS 下载时：anchor + --undefined 删除（实际调用链保留代码）；stubs 的 hardware_poll 接 STM32F4 RNG、platform_time 接 SD3078 RTC。

---

## 6. 待补

1. ✅ 同基线 ELF/map + TLS 配置 + sizeof 产物：`build/AppDebug/` + 配置 + `build/sizeof_extract.o`
2. ⏳ 真实握手记录（BCE OTA URL 抓包）
3. ⏳ 运行期峰值 + 栈高水位 + `xPortGetMinimumEverFreeHeapSize`

---

## 结论

| 区域 | 结论 |
|------|------|
| **FLASH** | **够**。含 TLS 实测剩 26.97KiB（增量 77.2KiB，非估算120） |
| **SRAM** | **不够**。握手峰值候选1 35.3KiB / 候选2 21.3KiB，超 C堆(11.15)/CCM(4.60) |
| **单次 in_buf** | 16429B > C堆 11.15KiB，确定放不下 |
| **候选2+heap_4** | 握手 21.3KiB < 空池 24KiB，唯一可能路径，需运行期测 RTOS 占用+栈高水位 |

**修正 v2/v3**：Flash 不缺（实测推翻"缺58KiB"）；RAM 缺口仍在但候选2走 heap_4 有可能。完整定论需补运行期三项。
