/*
 * network_upgrade.h —— M2 网络升级入口（MQTT 通知 + 授权 + 下载 + 结果上报）
 *
 * 依据《系统设计说明书》升级章节 & M4 README "网络入口与TF导入"：
 *   - M2 负责 MQTT 任务通知、下载来源与权限校验、文件下载及结果上报
 *   - M4 只接收包数据并协调认证/安装，不新增 Socket 监听、云平台、下载器
 *
 * 与 M4 升级框架的对接（M2 调用方，M4 已提供实现）：
 *   1. NetworkUpgrade_BindAuthorization(&authorization) —— 绑定真实通知许可查询
 *      App_Entry唯一绑定UpgradeEntryPolicy，按来源分派到TF/网络查询
 *   2. UpgradeQuiesce_Bind(NETWORK, &participant) —— 绑定网络门控（pause/settle/sync/resume）
 *   3. 升级执行任务串行调：
 *        UpgradeEntry_Begin → UpgradeEntry_Write(循环) → UpgradeEntry_Finish → UpgradeEntry_Reboot
 *
 * 升级通知 JSON 格式（M2 定义，当前从commands topic接收）：
 *   {
 *     "task_id": "cloud-task-uuid",   // 云端任务号（供关联，不传给M4）
 *     "version": "1.2.3",              // 目标版本号（仅日志/上报，不传给 M4）
 *     "size": 262144,                  // 升级包总字节数（传给 Begin 的 total_size）
 *     "sha256": "hexstring64chars",    // 32B 传输摘要 hex（传给 Begin 的 sha256）
 *     "url": "https://host/firmware.gwp", // 下载URL，HTTPS执行服务器证书校验
 *     "token": "optional-auth-header"  // 可选下载认证头
 *   }
 *
 * 状态上报（M2 PUBLISH 到 ota_status topic）：
 *   {"task_id":"...","phase":"downloading","offset":12345,"total":262144}
 *   {"task_id":"...","phase":"verifying"}
 *   {"task_id":"...","phase":"ready"}
 *   {"type":"boot_result","tx_id":"0000000000000007","sequence":9,
 *    "phase":"confirmed","image_sha256":"64个小写hex字符"}  // 本次Boot已核验结果
 *   {"task_id":"...","phase":"failed","err":"E_INTEGRITY"}     // 失败
 * 启动结果phase为trial/confirmed/rolled_back；UNKNOWN不报终态。
 * tx_id是16个小写hex字符的持久事务号，sequence不是云端task_id，image_sha256是明文镜像摘要。
 * 网络任务空闲时重试至匹配PUBACK；云端任务关联/去重由M2及云端完成，不伪造task_id。
 */
#ifndef NETWORK_UPGRADE_H
#define NETWORK_UPGRADE_H

#include <stdbool.h>
#include <stdint.h>
#include "cmsis_os2.h"   /* osPriorityBelowNormal */
#include "upgrade_entry.h"
#include "app_modules.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 升级执行任务栈（6KiB CCM，UPGRADE_EXECUTOR_MIN_FREE_STACK_BYTES=4096） ===== */
#define NETWORK_UPGRADE_TASK_STACK_BYTES  (6U * 1024U)   /* 6KiB，README 最低要求 */
#define NETWORK_UPGRADE_TASK_PRIORITY     osPriorityNormal  /* 与TF/存储同级，认证期间也能按期报告进展 */

/* ===== 字符串缓冲容量含结尾NUL；超长字段拒绝，不截断 ===== */
#define NW_UPG_URL_MAX        128     /* 下载 URL 最多127 B */
#define NW_UPG_VERSION_MAX    32      /* 版本号最多31 B */
#define NW_UPG_TASK_ID_MAX    64      /* 云端任务号最多63 B */
#define NW_UPG_TOKEN_MAX      128     /* 认证 token 最多127 B */
#define NW_UPG_SHA256_HEX_LEN 64      /* SHA256 hex 字符串长度（32B → 64 hex char） */

/* ===== 对外接口 ===== */

typedef struct
{
    bool (*verify)(void *context, const char *json);
    void *context;
} NetworkUpgradeAuthorization;

/*
 * 初始化前在任务上下文绑定一次核验回调；回调/context在运行期间有效且不替换。
 * verify在MQTT任务的Notify中接收完整JSON，须严格校验消息结构、顶层字段、来源、目标、权限、时效及下载来源；
 * 只读、无I/O、不保留JSON指针，不得仅凭字段存在、连接状态或格式正确放行。
 * 通过后模块把许可绑定到本次非零epoch，后续入口只读查询该许可；未绑定拒绝接收。
 * MQTT撤权/许可到期时须调用Revoke；清理未确认或READY提交不明时不接下一包。
 */
UpgradeStageResult NetworkUpgrade_BindAuthorization(const NetworkUpgradeAuthorization *authorization);
// 供装配层唯一策略查询；只有当前网络执行任务及匹配的非零epoch能通过。
bool NetworkUpgrade_Authorize(const UpgradeEntryPeer *peer);
// 仅MQTT任务撤销当前通知，返回原peer的Disconnect结果；执行任务负责继续结算。
UpgradeStageResult NetworkUpgrade_Revoke(void);

/**
 * @brief  网络升级模块初始化（装配阶段调用，不创建任务）
 * @note   只创建原信号量并绑定UpgradeQuiesce(NETWORK)，失败不标记初始化成功。
 *         由App_ModulesInit调用；缺任务许可时仍可上报Boot结果，接收保持NOT_READY。
 */
AppModuleInitResult NetworkUpgrade_Init(void);

/**
 * @brief  执行网络升级及结果上报，由统一装配创建一次。
 * @note   原静态TCB和6 KiB CCM栈纳入模块监督，不另建任务。
 */
void NetworkUpgrade_Task(void *argument);

/**
 * @brief  MQTT 回调收到升级通知时调用（不阻塞，只解析+唤醒执行任务）
 * @param  json  待核验的完整通知（\0结尾），仅MQTT任务调用，返回后可复用缓冲
 * @return 0=已接受，-1=格式错误，-2=会话/清理占用，-3=未就绪/未授权/非MQTT任务/epoch耗尽
 * @note   受理前检查task_id/version/url非空、字段无截断、包长及64个hex字符的摘要；token可缺省。
 *         当前字段采用无转义文本；边界检查不代替verify的完整消息校验。
 *         核验后把字段及32B摘要存到原静态结构体并唤醒执行任务，不在回调中下载或写Flash。
 */
int NetworkUpgrade_Notify(const char *json);

/**
 * @brief  查询当前升级状态字符串（供 MQTT 周期上报）
 * @return 静态字符串指针（idle/downloading/verifying/ready/rebooting/installed/failed/trial/rolled_back）
 */
const char *NetworkUpgrade_GetPhaseString(void);

/**
 * @brief  查询网络业务是否被门控暂停（升级期间）
 * @return true=已请求暂停，false=未请求；不代替任务停稳确认。
 * @note   停稳由quiesce_settle核对MQTT/Telemetry自身确认；保活和ota_status继续运行。
 */
bool NetworkUpgrade_IsQuiesced(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_UPGRADE_H */
