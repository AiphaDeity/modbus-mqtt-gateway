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

#include "cmsis_os2.h"

osMutexId_t g_m3_i2c1_mutex = NULL;

/* 底层介质初始化结果，供 Task 上报就绪位使用 */
static bool s_flash_ok = false;

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

    /* 2. 上报就绪位 */

    // 配置存储服务就绪：判据是 SPI Flash 可读写，空设备也算就绪
    App_SetResourceReady(APP_RESOURCE_STORAGE, s_flash_ok);

    // 时间服务就绪（RTC 在走就行，时间准不准由上层校时）
    App_SetResourceReady(APP_RESOURCE_TIME, storage_time_is_valid());

    // 控制状态恢复（日志开关、去重等，暂时直接就绪）
    App_SetResourceReady(APP_RESOURCE_CONTROL_STATE, true);

    /* 3. 业务循环 */
    for (;;)
    {
        // TODO: 后续在这里处理：
        //   - 配置保存请求队列
        //   - RTC 校时同步
        //   - TF 卡记录写入
        //   - 看门狗喂狗

        osDelay(1000);
    }
}
