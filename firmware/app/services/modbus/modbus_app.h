#ifndef __MODBUS_APP_H__
#define __MODBUS_APP_H__
#include <stdint.h>
#include "modbus.h"
#include "mb_pointmap.h"

/* ========= 前向声明 ========= */
typedef struct mb_slave_runtime mb_slave_runtime_t;


#define MB_REG_MAX_PER_SLAVE     32U   // 每一个从站，最多存32个寄存器
#define MB_SLAVE_INIT_CAP        4U    // 初始容量，和扩容粒度一致，一次+4
#define MB_SLAVE_HARD_MAX        16U   // 最多可以挂16个Modbus从站
#define MB_WRITE_QUEUE_DEPTH     4U    // 外部写请求队列深度（MQTT下发配置/运行写操作）

/* ============= 日志等级控制（量产时设为 MB_LOG_LEVEL_OFF 关掉所有日志）============= */
#define MB_LOG_LEVEL_OFF    0   // 关闭所有日志
#define MB_LOG_LEVEL_ERROR  1   // 只打错误
#define MB_LOG_LEVEL_WARN   2   // 错误 + 警告
#define MB_LOG_LEVEL_INFO   3   // 错误 + 警告 + 一般信息
#define MB_LOG_LEVEL_DEBUG  4   // 全部

#ifndef MB_LOG_LEVEL
    #define MB_LOG_LEVEL  MB_LOG_LEVEL_INFO   // ← 这里量产前改成 MB_LOG_LEVEL_OFF
#endif

// 【解耦层】只改这一处就能替换底层输出，比如改成rt_kprintf、自定义串口函数
#define MB_LOG_OUT(fmt, ...)    printf(fmt, ##__VA_ARGS__)

#if MB_LOG_LEVEL >= MB_LOG_LEVEL_ERROR
    #define MB_LOGE(fmt, ...)   MB_LOG_OUT("[MB-E] %s:%d " fmt "\r\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define MB_LOGE(fmt, ...)   ((void)0)
#endif

#if MB_LOG_LEVEL >= MB_LOG_LEVEL_WARN
    #define MB_LOGW(fmt, ...)   MB_LOG_OUT("[MB-W] %s:%d " fmt "\r\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define MB_LOGW(fmt, ...)   ((void)0)
#endif

#if MB_LOG_LEVEL >= MB_LOG_LEVEL_INFO
    #define MB_LOGI(fmt, ...)   MB_LOG_OUT("[MB-I] %s:%d " fmt "\r\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define MB_LOGI(fmt, ...)   ((void)0)
#endif

#if MB_LOG_LEVEL >= MB_LOG_LEVEL_DEBUG
    #define MB_LOGD(fmt, ...)   MB_LOG_OUT("[MB-D] %s:%d " fmt "\r\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define MB_LOGD(fmt, ...)   ((void)0)
#endif

/* ============= 配置变更回调类型 ============= */
/**
 * @brief 从站配置变更原因
 */
typedef enum
{
    MB_CFG_CHANGE_ADD     = 0,   // 新增从站
    MB_CFG_CHANGE_UPDATE  = 1,   // 更新已有从站配置（ID 重复时）
    MB_CFG_CHANGE_DELETE  = 2,   // 删除从站
    MB_CFG_CHANGE_RELOAD  = 3,   // 批量重载（上电时调用 mb_handle_init + 逐个 add 之前）
} mb_cfg_change_reason_t;

/**
 * @brief 配置变更回调函数类型
 *
 *  存储模块通过 mb_set_config_change_cb() 注册，Modbus 在从站增删改成功后调用。
 *  典型用法：回调里遍历 mb_handle_get()->slave_list 写 Flash。
 *
 * @param reason  变更原因
 * @param slave_id 变更涉及的从站 ID（批量重载时为 0）
 * @param user_ctx 用户传入的上下文指针（可选）
 */
typedef void (*mb_cfg_change_cb_t)(mb_cfg_change_reason_t reason,
                                    uint8_t slave_id,
                                    void *user_ctx);

/**
 * @brief 从站状态机
 */
typedef enum
{
    MB_STATE_IDLE = 0,        // 空闲，可以挑选下一个从站下发
    MB_STATE_TX,              // 正在发送（等待发送完成回调）
    MB_STATE_WAIT_ACK,        // 发送完成，等待应答帧 + 超时判断
    MB_STATE_ERR              // 异常兜底（可选）
} mb_state_t;

typedef enum
{
    MB_STATUS_OK = 0,    // 成功
    MB_INIT_STATUS_ERR,   // 初始化失败
    MB_REALLOC_STATUS_ERR,   // 扩容失败
    MB_HANDLE_STATUS_ERR,   // 句柄不存在
    MB_SLAVE_NOT_FOUND_ERR,   // 从站不存在
    MB_PARSE_STATUS_ERR,   // 解析失败
    MB_BUILD_REQUEST_ERR,   // 构建请求帧失败
    MB_SEND_REQUEST_ERR,   // 发送请求帧发送失败
    MB_QUEUE_FULL_ERR,     // 外部写请求队列满
    MB_NOT_SUPPORT_FC_ERR, // 不支持的功能码（只接受 FC6/FC16）
} mb_status_e;

/**
 * @brief 外部写请求消息体
 *
 *  外部任务（MQTT、配置管理等）通过 mb_inject_write() 投递到此队列，
 *  由 poll 任务在 IDLE 状态优先取出，用 ModbusQueryInject 头插发送。
 *
 *  写入值直接拷贝到 write_values[]，避免外部指针生命周期问题。
 *  result_sem 可选：调用者传 osSemaphoreId_t 用于同步等待结果；
 *                NULL 表示异步，不通知调用者。
 */
typedef struct
{
    uint8_t        slave_id;      // 目标从站地址 (1~247)
    uint8_t        func_code;     // MB_FC_WRITE_REGISTER(6) / MB_FC_WRITE_MULTIPLE_REGISTERS(16)
    uint16_t       reg_addr;      // 起始寄存器地址
    uint16_t       reg_count;     // 写入数量 (FC6 固定为1)
    uint16_t       write_values[MB_REG_MAX_PER_SLAVE];  // 写入值（直接拷贝，无指针风险）

    osSemaphoreId_t result_sem;   // 可选：结果通知信号量；NULL 表示异步
    mb_errot_t     *result_err_out; // 可选：结果输出指针；poll 任务处理完后填入
} mb_write_request_t;

/**
 * @brief 从站数据快照（MQTT 方安全读取用）
 *
 *  外部模块通过 mb_take_slave_snapshot() 获取，内部已加锁拷贝，
 *  拿到后可以自由读写，与 Modbus poll 任务完全解耦。
 */
typedef struct
{
    uint8_t  slave_id;           // 从站地址
    uint8_t  is_online;          // 在线状态（poll 成功置 1，超时置 0）
    uint8_t  data_valid;         // 数据新鲜度（1=本次周期 poll 成功；0=无新数据）
    uint16_t reg_addr;           // 寄存器起始地址
    uint16_t valid_point;        // 有效数据长度
    uint16_t data[MB_REG_MAX_PER_SLAVE];  // 数据快照（拷贝而非指针）

    uint32_t   last_collect_tick;          // 最后一次采集（成功或失败）的 tick
    mb_errot_t collect_status;             // 最后一次采集结果（ERR_OK_QUERY 等）
} mb_slave_snapshot_t;

/**
 * @brief 遍历回调类型（mb_for_each_valid_slave 使用）
 *
 * @param slave  当前遍历到的从站（已保证 valid && enable）
 * @param ctx    用户传入的上下文指针
 * @return 0=继续遍历；非0=立即停止遍历
 */
typedef int (*mb_slave_iter_cb_t)(mb_slave_runtime_t *slave, void *ctx);

struct mb_slave_runtime
{
    uint8_t valid;                                   // 0空位；1有效从站
    modbus_t telegram;

    uint32_t last_trigger_tick;                      // 上次触发采集时间
    uint8_t  fail_count;                             // 连续失败次数
    uint8_t  consecutive_fail;                       // ✅ 连续失败阶梯（阶梯冷却用）  

    uint16_t reg_data_buf[MB_REG_MAX_PER_SLAVE];     // 寄存器数据缓存
    uint16_t valid_point;                            // 缓存有效数据长度（经计算）
    uint8_t  is_online;                              // 是否在线
    mb_errot_t collect_status;                       // 采集状态

    uint8_t data_valid;                            // 0无效数据；1有效数据

    mb_point_t points[MB_POINTS_PER_SLAVE];        // 点位表（配置下发/FROM Flash 填充）
};

/**
 * @brief Modbus模块总句柄
 */
typedef struct
{
    mb_slave_runtime_t *slave_list;
    uint8_t slave_max_num;
    uint8_t slave_actual_num;

    // ===== 全局轮询状态（单槽一问一答核心状态）=====
    mb_state_t state;                   // 状态机
    mb_slave_runtime_t *waiting_slave; // 当前正在等待应答的从站对象（读请求）
    uint8_t  waiting_is_write;         // 1=当前等待的是外部写请求；0=正常读轮询
    mb_write_request_t *pending_write; // 仅当 waiting_is_write==1 时有效
    uint8_t  idle_no_work;             // poll 任务内部标志：本轮 IDLE 是否无事可做（用于决定 osDelay）

    // ===== 外部接口 =====
    osMessageQueueId_t xWriteReqQueue; // 外部写请求队列（MQTT下发配置/运行写操作）
    mb_cfg_change_cb_t  cfg_change_cb; // 配置变更回调（存储模块注册）
    void              *cfg_change_ctx; // 回调上下文指针

    osMutexId_t xSlaveDataMutex;       // 数据缓存读写互斥（modbus写 / mqtt读快照）
    osMutexId_t xSlaveCfgMutex;         // 从站列表增删改互斥
} mb_handle_t;


void Modbus_Init(void);
void vModbusPollTask(void* argument);

mb_status_e mb_handle_init(void);
mb_status_e mb_realloc_slave_list(void);
mb_status_e mb_poll_slave(void);
mb_status_e mb_add_slave(modbus_t *cfg);
mb_slave_runtime_t* mb_get_slave_runtime(uint8_t slave_id);
mb_status_e mb_delete_slave(uint8_t slave_id);

/**
 * @brief 获取 Modbus 模块总句柄（用于 MQTT 遍历从站列表、Flash 持久化等外部模块）
 */
mb_handle_t* mb_handle_get(void);

/**
 * @brief 注册从站配置变更回调
 *
 *  存储模块（Flash持久化）在系统初始化时调用一次。
 *  之后 mb_add_slave / mb_delete_slave 成功时自动触发回调，
 *  回调里遍历 mb_handle_get()->slave_list 写 Flash 即可。
 *
 * @param cb    回调函数指针（传 NULL 可取消注册）
 * @param ctx   用户上下文指针，回调时原样传回（可选，传 NULL 即可）
 */
void mb_set_config_change_cb(mb_cfg_change_cb_t cb, void *ctx);

/**
 * @brief 外部注入写请求（FC6 或 FC16）
 *
 *  由 MQTT 任务/配置管理任务调用，请求会投入队列，
 *  poll 任务在 IDLE 状态优先取出，用 ModbusQueryInject 头插发送。
 *
 * @param slave_id       目标从站 (1~247)
 * @param func_code      MB_FC_WRITE_REGISTER(6) / MB_FC_WRITE_MULTIPLE_REGISTERS(16)
 * @param reg_addr       起始寄存器地址
 * @param reg_count      FC6 固定传 1；FC16 传写入数量 (≤ MB_REG_MAX_PER_SLAVE)
 * @param write_values   要写入的值数组（直接拷贝，无指针生命周期问题）
 * @param result_sem     可选：调用者传入信号量，写完成后 poll 任务 release，
 *                       调用者用 osSemaphoreAcquire 同步等待结果；
 *                       传 NULL 表示异步，不通知
 * @param result_err_out 可选：调用者传入 mb_errot_t*，写完成后填入结果，
 *                       配合 result_sem 使用；传 NULL 忽略
 * @return MB_STATUS_OK 成功投递；MB_QUEUE_FULL_ERR 队列满；MB_NOT_SUPPORT_FC_ERR 功能码不支持
 */
mb_status_e mb_inject_write(uint8_t slave_id,
                            uint8_t func_code,
                            uint16_t reg_addr,
                            uint16_t reg_count,
                            const uint16_t *write_values,
                            osSemaphoreId_t result_sem,
                            mb_errot_t *result_err_out);

/* ============= MQTT 方 / 存储方 统一接口 ============= */

/**
 * @brief 遍历所有有效且启用的从站（MQTT 发布 / 存储遍历都可以用）
 *
 *  自动过滤 valid==1 && telegram.enable==1 的从站，
 *  内部已加 xSlaveCfgMutex 配置锁，回调里不要再调 mb_add_slave/mb_delete_slave 以免死锁。
 *
 * @param cb   遍历回调，返回 0 继续，非 0 立即停止
 * @param ctx  可选上下文指针，回调原样传回（传 NULL 即可）
 * @return 遍历到的从站数量
 */
uint8_t mb_for_each_valid_slave(mb_slave_iter_cb_t cb, void *ctx);

/**
 * @brief 安全获取某个从站的数据快照（MQTT 方推荐用这个，别自己拿 mutex）
 *
 *  内部加 xSlaveDataMutex 锁后一次性拷贝数据，返回后调用者可自由使用，
 *  与 Modbus poll 任务完全解耦。
 *
 * @param slave_id 从站地址
 * @param snap_out 输出快照结构体（调用者分配，内部填值）
 * @return MB_STATUS_OK 成功；MB_SLAVE_NOT_FOUND_ERR 从站不存在或未启用
 */
mb_status_e mb_take_slave_snapshot(uint8_t slave_id, mb_slave_snapshot_t *snap_out);

/**
 * @brief 通知存储方上电批量加载完成
 *
 *  存储方上电时：先 Modbus_Init → 逐个 mb_add_slave 从 Flash 加载 → 最后调用本函数。
 *  本函数会触发一次 MB_CFG_CHANGE_RELOAD 回调，让存储方完成最后的收尾（如 CRC 校验）。
 */
void mb_notify_reload_complete(void);


#endif