#include "storage.h"
#include "app_readiness.h"
#include "main.h"

#include "rtc_sd3078.h"
#include "spi_flash_w25q128.h"
#include "sdcard_tf.h"
#include "storage_config.h"
#include "storage_time.h"
#include "storage_seq.h"

#include "cmsis_os2.h"

/* 配置 JSON 缓冲区（64KiB，最大配置大小） */
static uint8_t s_config_json[64 * 1024];
static uint32_t s_config_json_len = 0;

osMutexId_t g_m3_i2c1_mutex = NULL;

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
    (void)spi_flash_w25q128_init();

    // TF 卡 — 可能没插卡，失败正常
    (void)sdcard_tf_init();

    // 序列号管理 — 从 Flash 恢复高水位
    (void)storage_seq_init();

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

    /* 1. 加载配置（从 W25Q128 三槽中选最新有效 JSON） */
    (void)storage_config_load(s_config_json, sizeof(s_config_json),
                               &s_config_json_len);

    /* 2. 上报就绪位 */

    // 配置存储服务就绪（SPI Flash 初始化成功 + 配置已加载）
    App_SetResourceReady(APP_RESOURCE_STORAGE, true);

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
