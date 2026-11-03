#ifndef MODBUS_APP_H
#define MODBUS_APP_H
#include "cmsis_os2.h"

#define MODBUS_FRAME_MAX_LEN     256U    // 最大帧长度
//#define MODBUS_FRAME_POOL_NUM    8U      //帧池槽数量
#define MB_REG_MAX_PER_SLAVE     32U   // 每一个从站，最多存32个寄存器
#define MB_SLAVE_HARD_MAX        16U   // 最多可以挂16个Modbus从站
#define MB_SLAVE_INIT_CAP        4U    // 初始容量，和扩容粒度一致，一次+4


/*
1. 485硬件收到应答 → DMA搬运 → dma_rx_buf
2. 空闲中断回调：把dma_rx_buf的数据拷贝进 frame_pool 空闲slot，valid=1
3. 释放 xNewFrameNotifySem 信号量
4. vModbusPollTask 被唤醒
5. 任务找到valid=1的槽，取出原始报文
6. 调用modbus_proto解析CRC、寄存器数据
7. 【原始帧用完】slot标记 valid=0，归还帧池，可以接收下一条应答
8. 【解析结果】写入对应 slave_runtime_t 的 reg_data_buf、状态、计数
9. 生成 mb_slave_snapshot_t 快照
10. 快照投递到MQTT消息队列，MQTT任务消费、上传云端
11. 轮询任务回到循环，继续：定时发起下一次采集 / 等待新应答 / 做超时判断
*/

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


/**
 * @brief 从站静态配置，持久化存储主体
 */
typedef struct
{
    uint8_t  slave_id;                  // 从站ID
    uint8_t  func_code;                 // 功能码 FuncCode_t
    uint16_t reg_start_addr;            // 起始地址（线圈地址/寄存器地址，由func_code决定）
    uint16_t reg_cnt;                   // 采集点数（线圈数量/寄存器数量）
    uint32_t collect_interval_ms;       // 采集间隔，ms
    uint8_t  enable;                    // 0禁用；1启用
    uint8_t  retry_cnt;                 // 重试次数
} mb_slave_cfg_t;

/**
 * @brief 接收帧单槽
 * valid 所有权标记：0空闲；1槽内报文完整有效
 */
typedef struct
{
    uint8_t  data[MODBUS_FRAME_MAX_LEN];
    uint16_t frame_len;
    uint32_t recv_tick;
    uint8_t  valid;
} mb_frame_slot_t;

/**
 * @brief 从站运行时对象：仅保存属于这个从站自身的缓存与统计
 * 【不再存放等待应答状态，等待状态全局唯一，放在mb_handle】
 */
typedef struct
{
    uint8_t valid;                                   // 0空位；1有效从站
    mb_slave_cfg_t cfg;
    uint32_t last_trigger_tick;                      // 上次触发采集时间
    uint8_t  fail_count;                             // 连续失败次数
    uint8_t  consecutive_fail;                         // ✅ 连续失败阶梯（阶梯冷却用）  

    uint16_t reg_data_buf[MB_REG_MAX_PER_SLAVE];     // 寄存器数据缓存
    uint16_t valid_point;                           // 缓存有效数据长度（经计算）
    uint8_t  is_online;                             // 是否在线
    uint8_t collect_status;                         // 0:正常, 1:异常应答, 2:超时, 3:CRC错误

    // ===== 新增：预编译好的请求帧，cfg不变则永远不变 =====
    uint8_t data_valid;                            // 0无效数据；1有效数据
    uint8_t tx_req_buf[MODBUS_FRAME_MAX_LEN];
    uint16_t tx_req_len;
} mb_slave_runtime_t;

/**
 * @brief 对外输出给MQTT的数据快照（视图层）
 * 注意：reg_data 只是指针，快照使用期间必须持有 xSlaveDataMutex
 */
// typedef struct
// {
//     uint8_t  slave_id;
//     uint8_t  enable;
//     uint8_t  is_online;
//     uint32_t last_collect_tick;
//     uint16_t reg_start_addr;
//     uint16_t reg_cnt;
//     uint16_t *reg_data;
// } mb_slave_snapshot_t;

/**
 * @brief Modbus模块总句柄
 */
typedef struct
{
    mb_slave_runtime_t *slave_list;
    uint8_t slave_max_num;
    uint8_t slave_actual_num;

    // 全局唯一接收槽
    mb_frame_slot_t frame_slot;
    uint8_t dma_rx_buf[MODBUS_FRAME_MAX_LEN];
    uint8_t tx_busy;                    // 485半双工发送忙标记

    // ===== 全局轮询状态（单槽一问一答核心状态）=====
    mb_state_t state;                   // ✅ 状态机
    mb_slave_runtime_t *waiting_slave; // 当前正在等待应答的从站对象
    uint32_t send_tick;                 // 请求发送时刻tick
    uint32_t timeout_ms;                // 本次等待超时

    osThreadId_t poll_task_handle;
    osSemaphoreId_t xNewFrameNotifySem;
    osMutexId_t xSlaveDataMutex;       // 数据缓存读写互斥（modbus写 / mqtt读快照）
    osMutexId_t xSlaveCfgMutex;         // 从站列表增删改互斥
} mb_handle_t;

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
} mb_status_e;


// 二值信号量：收到新帧通知
extern osSemaphoreId_t new_frame_notify_sem;
// 互斥锁：从站运行数据（寄存器、在线状态，高频读写）
extern osMutexId_t slave_data_mutex;
// 互斥锁：从站静态配置（slave_id、地址、间隔、使能，低频改）
extern osMutexId_t slave_cfg_mutex;


mb_status_e mb_handle_init(void);
mb_status_e mb_realloc_slave_list(void);
mb_slave_runtime_t* mb_get_slave_runtime(uint8_t slave_id);
mb_status_e mb_in_slot(uint16_t len);

uint8_t mb_fetch_all_frame(mb_frame_slot_t local_frames[], uint8_t *local_frame_cnt);

mb_status_e mb_delete_slave(uint8_t slave_id);
mb_status_e mb_parse_frame(mb_frame_slot_t *local_frame);
mb_status_e mb_send_slave_request(mb_slave_runtime_t *slave);

static uint32_t mb_get_backoff_cooling_ms(uint8_t consecutive_fail);



#endif
