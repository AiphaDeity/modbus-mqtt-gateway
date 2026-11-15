#ifndef BOOT_INSTALL_H
#define BOOT_INSTALL_H

#include "boot_package_flash.h"
#include "boot_upgrade_state.h"

/*
 * 安装接口在 Boot 主线程同步调用，HAL 时基须运行，调用方独占 SPI3 和 App Flash。
 * 源包及可信公钥 65 B/AES 密钥 32 B 在调用期间有效且不变；接口不保留指针。
 */
typedef enum
{
  BOOT_INSTALL_BOOT_EXISTING,
  BOOT_INSTALL_TRIAL_STARTED,
  BOOT_INSTALL_RESTORED,
  BOOT_INSTALL_STATE_ERROR,
  BOOT_INSTALL_SOURCE_ERROR,
  BOOT_INSTALL_APP_ERROR,
  BOOT_INSTALL_NO_RECOVERY
} Boot_InstallResult;

/*
 * 仅空白升级日志允许首次建档：认证包，并比较当前 App 的长度范围内摘要和向量。
 * source->address 必须为 A/B 槽起点，source->size 为完整包字节数；不擦写片内 App。
 * tx_id须在收包前持久预留；失败不能回收已消耗的ID。
 * 成功返回 BOOT_INSTALL_BOOT_EXISTING，其余返回失败原因；不自动信任已存在包。
 */
Boot_InstallResult Boot_InstallEnroll(const Boot_PackageFlashSource *source,
                                     const uint8_t *public_key, const uint8_t *aes_key, uint64_t tx_id);

/*
 * 根据日志完成安装、重装、回退或恢复，不自行跳转 App。
 * 仅 BOOT_EXISTING、TRIAL_STARTED、RESTORED 允许继续最终认证和跳转检查。
 * TRIAL_STARTED 只允许本次启动；最多放行两次，每次都须先持久记录次数。
 * 健康确认仅接受与当前事务匹配的软件复位请求。
 */
Boot_InstallResult Boot_InstallRun(const uint8_t *public_key, const uint8_t *aes_key);

/*
 * 完整认证 V2/IWDG_HANDOFF 包及向量，成功输出槽号、整包长度和明文摘要。
 * package 须可写，失败时不可使用；capabilities 可为 NULL，非 NULL 时失败置零。
 * 输出不得与输入或彼此重叠；认证成功不表示 App 已安装或健康确认。
 */
bool Boot_InstallVerifySource(const Boot_PackageFlashSource *source, const uint8_t *public_key,
                              const uint8_t *aes_key, Boot_UpgradePackage *package,
                              uint16_t *capabilities);

/*
 * 普通上下文只读检查片内实际固件长度范围内的摘要和向量，不改变持久状态。
 * 固定30000 ms软件上限，256 B块读出并散列后报告进展；非法参数或超时返回false。
 */
bool Boot_InstallAppMatches(const Boot_UpgradePackage *package);

/*
 * 擦写并校验片内App；必须先完整认证源包并提交持久安装/恢复事务。
 * 调用期间独占源包和App Flash；source与package描述同一包，指针不保留。
 * 尾字不足4B填FF，摘要不含填充；失败可能已部分擦写，禁止跳转App。
 * 擦除阶段固定30000 ms，认证/解密输出共用180000 ms，最终摘要比对另限30000 ms。
 * 各阶段起点不随进展重置；到期锁存监督失败，调用方须复位而不能继续恢复写入。
 * 这些是软件保护上限，同Bank擦除时基停顿及硬件超时预算仍须单独核算。
 */
bool Boot_InstallImage(Boot_PackageFlashSource *source, const Boot_UpgradePackage *package,
                        const uint8_t *public_key, const uint8_t *aes_key);

#endif
