#include "storage.h"
#include "app_readiness.h"
#include "main.h"
#include "usart.h"   /* huart1 */

#include <stdio.h>   /* 业务级日志（Gateway boot / 热插拔事件） */
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
#include "storage_m2.h"
#include "m3_selftest.h"

#include "cmsis_os2.h"

osMutexId_t g_m3_i2c1_mutex = NULL;

/* 底层介质初始化结果，供 Task 上报就绪位使用 */
static bool s_flash_ok = false;
static bool s_floor_ok = false;
static bool s_log_ok = false;

/* ---- PD3 EXTI 热插拔中断 ----
 *
 * 卡座 PD3：卡插入→LOW，卡拔出→HIGH。
 * 用 EXTI line 3 双边沿捕获，确保即使主循环被 HAL_SD_Init 阻塞
 * 也不会错过物理插拔事件。中断里只设标志，主循环处理。
 */
#define SD_CARD_DET_IRQn EXTI3_IRQn

static volatile bool s_cd_event_flag = false;   /* 中断里置位 */

/* 中断回调：HAL_GPIO_EXTI_IRQHandler(EXTI_LINE_x) → HAL_GPIO_EXTI_Callback
 *
 * 注意：中断上下文里不能跑 HAL_SD_Init 这种重活！
 * 所以这里只置标志，真正的处理在主循环里做。
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == SD_CARD_DET_Pin) {
        s_cd_event_flag = true;
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_Pin);
    }
}

/* 在 Init 里配置 EXTI */
static void sdcard_detect_exti_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* 重新配置 PD3：从 GPIO_MODE_INPUT 改为 GPIO_MODE_IT_RISING_FALLING
     * 这样 PD3 上任何电平变化（LOW→HIGH 或 HIGH→LOW）都会触发 EXTI3。
     */
    GPIO_InitStruct.Pin = SD_CARD_DET_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(SD_CARD_DET_GPIO_Port, &GPIO_InitStruct);

    /* HAL 内部会自动把 GPIO_Pin=BIT3 连接到 EXTI line 3，
     * 但我们需要手动 enable NVIC。
     */
    HAL_NVIC_SetPriority(SD_CARD_DET_IRQn, 5, 0);  /* 优先级 5 */
    HAL_NVIC_EnableIRQ(SD_CARD_DET_IRQn);

    s_cd_event_flag = false;   /* 清 pending */
    __HAL_GPIO_EXTI_CLEAR_IT(SD_CARD_DET_Pin);
}

/* EXTI line 3 IRQ handler — 覆盖 startup.s 里的 weak Default_Handler */
void EXTI3_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(SD_CARD_DET_Pin);
}

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

    // PD3 EXTI 热插拔：让插拔事件在任何时候都能被捕获
    sdcard_detect_exti_init();

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

/*
 * TF 卡物理检测（PD3）：
 *   - 卡插入 → PD3 低电平 (GPIO_PIN_RESET)
 *   - 卡拔出 → PD3 高电平 (GPIO_PIN_SET)
 * 卡座机械开关 + 内部上拉 (gpio.c 已改 PULLUP)。
 */
static bool sdcard_physical_present(void)
{
    return (HAL_GPIO_ReadPin(SD_CARD_DET_GPIO_Port, SD_CARD_DET_Pin) == GPIO_PIN_RESET);
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

    /* M2↔M3 队列协议（写队列/补传/回执），队列句柄先于卡就绪创建 */
    storage_m2_init();

    /* 2. 上报就绪位 */

    // 配置存储服务就绪：判据是 SPI Flash 可读写，空设备也算就绪
    App_SetResourceReady(APP_RESOURCE_STORAGE, s_flash_ok);

    // 时间服务就绪（RTC 在走，且时间不早于持久下界）
    App_SetResourceReady(APP_RESOURCE_TIME, storage_time_is_valid());

    // 控制状态恢复：持久时间下界 + 控制日志（去重表）均已恢复
    App_SetResourceReady(APP_RESOURCE_CONTROL_STATE, s_floor_ok && s_log_ok);

#if M3_SELFTEST_ENABLE
    // 上板自检：把各层状态一次性打印到日志串口（USART1, 115200）
    m3_selftest_run();
#endif

    /* 3. 业务循环 */
    uint32_t last_floor_tick = HAL_GetTick();
    uint32_t last_cd_poll_tick = HAL_GetTick();
    bool last_card_present = sdcard_physical_present();

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

        // M2↔M3 队列：写请求 / 补传拉取 / ack 回执
        storage_m2_process();

        // 记录环状态节流落盘（段切换时已在 append 内立即落盘）
        storage_record_maintenance();

        /*
         * TF 卡热插拔：
         *   - EXTI 中断优先（PD3 任何边沿变化都设 s_cd_event_flag）
         *   - 2s 轮询兜底（防止中断偶发丢失）
         */
        bool now_present = sdcard_physical_present();
        bool cd_event = false;

        if (s_cd_event_flag) {
            /* EXTI 捕获到了物理插拔边缘 */
            s_cd_event_flag = false;
            cd_event = true;
        } else if ((HAL_GetTick() - last_cd_poll_tick) >= 2000U) {
            /* 2s 轮询兜底 */
            last_cd_poll_tick = HAL_GetTick();
            cd_event = (now_present != last_card_present);
        }

        if (cd_event) {
            if (!now_present && last_card_present) {
                /* LOW→HIGH：检测到卡拔出 */
                printf("[M3] TF card removed (PD3=HIGH) unmounting...\r\n"); fflush(stdout);
                storage_record_unmount();
                (void)sdcard_tf_deinit();
                printf("[M3] TF card unmounted + SDIO deinit done\r\n"); fflush(stdout);
            } else if (now_present && !last_card_present) {
                /* HIGH→LOW：检测到卡插入 */
                printf("[M3] TF card inserted (PD3=LOW) mounting...\r\n"); fflush(stdout);
                uint32_t t0 = HAL_GetTick();
                HAL_StatusTypeDef st = storage_record_init();
                uint32_t dt = HAL_GetTick() - t0;
                bool ready = storage_record_is_ready();
                printf("[M3] TF card mount %s (init_st=%d dt=%lums)\r\n",
                       ready ? "OK" : "FAIL", st, (unsigned long)dt); fflush(stdout);
            }
            last_card_present = now_present;
        } else if (now_present && !storage_record_is_ready()) {
            /* 卡物理在场但逻辑没 ready → 兜底重试（每 10s 一次） */
            static uint32_t last_retry_tick = 0;
            if ((HAL_GetTick() - last_retry_tick) >= 10000U) {
                last_retry_tick = HAL_GetTick();
                printf("[M3] TF card present but not ready, retry init\r\n"); fflush(stdout);
                uint32_t t0 = HAL_GetTick();
                (void)storage_record_init();
                uint32_t dt = HAL_GetTick() - t0;
                printf("[M3] retry init done dt=%lums ready=%d\r\n",
                       (unsigned long)dt, storage_record_is_ready()); fflush(stdout);
            }
        }

        osDelay(1000);
    }
}
