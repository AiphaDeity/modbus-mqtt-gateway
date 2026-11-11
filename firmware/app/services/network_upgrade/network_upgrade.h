/*
 * network_upgrade.h —— M2 网络升级入口（MQTT 通知 + 授权 + 下载 + 结果上报）
 *
 * 依据《系统设计说明书》升级章节 & M4 README "网络入口与TF导入"：
 *   - M2 负责 MQTT 任务通知、下载来源与权限校验、文件下载及结果上报
 *   - M4 只接收包数据并协调认证/安装，不新增 Socket 监听、云平台、下载器
 *   - network_upgrade 目录此前只有 .gitkeep，本模块填充 M2 侧实现
 *
 * 与 M4 升级框架的对接（M2 调用方，M4 已提供实现）：
 *   1. UpgradeEntry_BindPolicy(&policy)         —— 绑定权限策略（authorize 回调）
 *   2. UpgradeQuiesce_Bind(NETWORK, &participant) —— 绑定网络门控（pause/settle/sync/resume）
 *   3. Monitor_RegisterHealth(MQTT/TELEMETRY,..)  —— 登记健康检查
 *   4. 升级执行任务串行调：
 *        UpgradeEntry_Begin → UpgradeEntry_Write(循环) → UpgradeEntry_Finish → UpgradeEntry_Reboot
 *
 * 升级通知 JSON 格式（M2 定义，云端下发到 ota_task topic）：
 *   {
 *     "task_id": "cloud-task-uuid",   // 云端任务号（M2 用于去重，不传给 M4）
 *     "version": "1.2.3",              // 目标版本号（仅日志/上报，不传给 M4）
 *     "size": 262144,                  // 升级包总字节数（传给 Begin 的 total_size）
 *     "sha256": "hexstring64chars",    // 32B 传输摘要 hex（传给 Begin 的 sha256）
 *     "url": "http://host/firmware.gwp", // 下载 URL（HTTP，不支持 TLS）
 *     "token": "optional-auth-header"  // 可选下载认证头
 *   }
 *
 * 状态上报（M2 PUBLISH 到 ota_status topic）：
 *   {"task_id":"...","phase":"downloading","offset":12345,"total":262144}
 *   {"task_id":"...","phase":"verifying"}
 *   {"task_id":"...","phase":"ready"}
 *   {"task_id":"...","phase":"installed","result":"success"}   // 重启后
 *   {"task_id":"...","phase":"failed","err":"E_INTEGRITY"}     // 失败
 */
#ifndef NETWORK_UPGRADE_H
#define NETWORK_UPGRADE_H

#include <stdbool.h>
#include <stdint.h>
#include "cmsis_os2.h"   /* osPriorityBelowNormal */

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 升级执行任务栈（README 要求 ≥6KiB 普通SRAM，UPGRADE_EXECUTOR_MIN_FREE_STACK_BYTES=4096） ===== */
#define NETWORK_UPGRADE_TASK_STACK_BYTES  (8U * 1024U)   /* 8KiB，含 HTTP 下载 + SHA + 日志 */
#define NETWORK_UPGRADE_TASK_PRIORITY     osPriorityBelowNormal  /* 低于 MQTT/Telemetry，不抢业务 */

/* ===== 升级通知 JSON 字段上限（防止缓冲溢出） ===== */
#define NW_UPG_URL_MAX        128     /* 下载 URL 最大长度 */
#define NW_UPG_VERSION_MAX    32      /* 版本号最大长度 */
#define NW_UPG_TASK_ID_MAX    64      /* 云端任务号最大长度 */
#define NW_UPG_TOKEN_MAX      128     /* 认证 token 最大长度 */
#define NW_UPG_SHA256_HEX_LEN 64      /* SHA256 hex 字符串长度（32B → 64 hex char） */

/* ===== 对外接口 ===== */

/**
 * @brief  网络升级模块初始化（装配阶段调用，不创建任务）
 * @note   绑定 UpgradeEntryPolicy + UpgradeQuiesce(NETWORK) + Monitor 健康。
 *         绑定失败只记日志不阻断启动（升级不可用 ≠ 整机不可用）。
 *         必须在 App_ModulesInit 之后、App_ModulesStart 之前调用。
 */
void NetworkUpgrade_Init(void);

/**
 * @brief  创建升级执行任务（App_ModulesStart 后调用）
 * @note   使用静态 TCB + 8KiB 普通 SRAM 栈，不在装配表里。
 *         任务优先级 BelowNormal，不抢 MQTT/Telemetry 业务。
 */
void NetworkUpgrade_StartTask(void);

/**
 * @brief  MQTT 回调收到升级通知时调用（不阻塞，只解析+唤醒执行任务）
 * @param  json  升级通知 JSON 字符串（\0 结尾，生命周期到下次调用前有效）
 * @return 0=已接受并唤醒执行任务，-1=格式错误，-2=已有升级进行中，-3=执行任务未启动
 * @note   ★不在回调里执行下载/Flash/密码运算（README 明确要求）。
 *         只解析 JSON 存到静态结构体，释放信号量唤醒执行任务。
 */
int NetworkUpgrade_Notify(const char *json);

/**
 * @brief  查询当前升级状态字符串（供 MQTT 周期上报）
 * @return 静态字符串指针（idle/downloading/verifying/ready/rebooting/installed/failed）
 */
const char *NetworkUpgrade_GetPhaseString(void);

/**
 * @brief  查询网络业务是否被门控暂停（升级期间）
 * @return true=已暂停（Telemetry/MQTT 遥测应跳过），false=正常运行
 * @note   Telemetry_Task 主循环每轮开始检查此标志。MQTT 保活和 ota_status 不受影响。
 */
bool NetworkUpgrade_IsQuiesced(void);

/**
 * @brief  重启后调用：查 Boot 身份 + GetStatus，上报最终升级结果
 * @note   在 App_Entry 或 MQTT 上线后调用一次。读 App_UpgradeIsTrial() 判断是否升级后启动，
 *         调 UpgradeEntry_GetStatus() 查安装结果，PUBLISH 到 ota_status。
 */
void NetworkUpgrade_ReportFinalResult(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_UPGRADE_H */
