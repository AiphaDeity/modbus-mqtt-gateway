#include "storage.h"
#include "app_readiness.h"
#include "main.h"

#include "rtc_sd3078.h"
#include "spi_flash_w25q128.h"
#include "sdcard_tf.h"
#include "storage_config.h"
#include "storage_time.h"
#include "storage_seq.h"
#include "storage_identity.h"
#include "storage_timefloor.h"
#include "storage_ctrllog.h"
#include "storage_record.h"

#include "cmsis_os2.h"

osMutexId_t g_m3_i2c1_mutex = NULL;

/* 底层介质初始化结果，供 Task 上报就绪位使用 */
static bool s_flash_ok = false;
static bool s_floor_ok = false;
static bool s_log_ok = false;

AppModuleInitResult Storage_Init(void)
{
    /* 1. 创建 I2C1 总线互斥锁 */
    const osMutexAttr_t mutex_attr = {
        .name = "i2c1_mtx",
        .attr_bits = 0,
        .cb_mem = NULL,
        .cb_size = 0,
    };

    g_m3_i2c1_mutex = osMutexNew(&mutex_attr);
    if (g_m3_i2c1_mutex == NULL)
    {
        return APP_MODULE_INIT_FAILED;
    }

    /*
     * 2. 初始化底层驱动。
     * 注意：按 app_modules.h 约定，Init 阶段不能无限等待。
     *       每个驱动的 init 都有超时保护，失败不阻塞。
     */

    // RTC（SD3078）— 失败不致命，时间服务后续会检测
    (void)rtc_sd3078_init();

    // SPI Flash（W25Q128）— 配置存储依赖它，失败也不阻塞
    s_flash_ok = (spi_flash_w25q128_init() == HAL_OK);

    // TF 卡 — 可能没插卡，失败正常
    (void)sdcard_tf_init();

    // 序列号管理 — 从 Flash 恢复高水位（双份冗余取最大）
    (void)storage_seq_init();

    // 设备身份 — 读取双份 gateway_id，取有效且最新的一份
    (void)storage_identity_init();

    // 持久时间下界 — 读取双份，取较大的有效值
    s_floor_ok = s_flash_ok && (storage_timefloor_init() == HAL_OK);

    // 控制日志 — 扫描双区，恢复写入位置与去重键
    s_log_ok = s_flash_ok && (storage_ctrllog_init() == HAL_OK);

    /*
     * 注意：不在 Init 里加载配置。
     * 配置加载放到 Task 里做，因为配置加载可能需要等待 SPI Flash ready，
     * 而 Init 阶段不能阻塞。
     */

    return APP_MODULE_INIT_OK;
}

void Storage_Task(void *argument)
{
    (void)argument;

    /*
     * 1. 确认配置区状态。
     * M3 不再把整份 JSON 读进 RAM（最大 64KiB 会吃掉一半 RAM），
     * 这里只打开句柄以触发运行槽选择；真正的字节流由配置维护模块
     * 按块调用 storage_config_read_chunk() 读取。
     */
    storage_config_reader_t cfg_rd;
    (void)storage_config_open(&cfg_rd);

    // 启动时用当前 RTC 时间推进持久下界（不足阈值时内部自动跳过）
    uint32_t now = storage_get_unix_time();
    if (now != 0)
        (void)storage_timefloor_update(now);

    /* 记录持久化（TF 卡）— 可能需要挂载/格式化，放在 Task 里不阻塞 Init */
    (void)storage_record_init();

    /* 2. 上报就绪位 */

    // 配置存储服务就绪：判据是 SPI Flash 可读写，空设备也算就绪
    App_SetResourceReady(APP_RESOURCE_STORAGE, s_flash_ok);

    // 时间服务就绪（RTC 在走，且时间不早于持久下界）
    App_SetResourceReady(APP_RESOURCE_TIME, storage_time_is_valid());

    // 控制状态恢复：持久时间下界 + 控制日志（去重表）均已恢复
    App_SetResourceReady(APP_RESOURCE_CONTROL_STATE, s_floor_ok && s_log_ok);

    /* 3. 业务循环 */
    uint32_t last_floor_tick = HAL_GetTick();

    for (;;)
    {
        // 每小时推进一次持久时间下界，保证长期运行也能检测到时间回拨
        if ((HAL_GetTick() - last_floor_tick) >= (STORAGE_TIMEFLOOR_STEP_SEC * 1000U))
        {
            last_floor_tick = HAL_GetTick();
            uint32_t t = storage_get_unix_time();
            if (t != 0)
                (void)storage_timefloor_update(t);
        }

        // 推进控制日志整区回收（无回收任务时立即返回）
        (void)storage_ctrllog_maintenance();

        // TODO: 后续在这里处理：
        //   - 配置保存请求队列
        //   - RTC 校时同步
        //   - TF 卡记录写入
        //   - 看门狗喂狗

        osDelay(1000);
    }
}
