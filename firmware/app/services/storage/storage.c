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
#include "storage_runtime.h"
#include "storage_upgrade.h"
#include "app_monitor.h"
#include "storage_upgrade_q.h"
#include "storage_upgrade_tf.h"
#include "upgrade_storage.h"

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

/*
 * 由 App_ModulesInit 在启动任务中调用，创建队列、互斥锁并初始化基础驱动，不创建存储任务。
 * 成功仅表示完成基础装配；持久状态恢复和运行状态报告留给随后创建的 Storage_Task。
 */
AppModuleInitResult Storage_Init(void)
{
    /* 先创建升级通道，初始化失败不能留下没有消费者的空句柄。 */
    if (!storage_runtime_init() || !storage_upgrade_q_init() || !storage_upgrade_tf_init())
        return APP_MODULE_INIT_FAILED;
    /* 1. 创建 I2C1 总线互斥锁 
    * 作用：I2C1是共享总线，SD3078、AHT20、INA226、PCA9555多个外设共用这条总线
    * RTOS下多个任务同时访问I2C会出现总线冲突、数据错乱，所以用互斥锁保证同一时刻只有一个任务操作I2C1
    */
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

    // Flash 和 TF 的实际可用性在任务中继续检查，不能用这里的 INIT_OK 代替存储服务就绪。
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

/*
 * 由装配表经 App_ModuleTaskEntry 调度进入，作为运行期存储所有者访问介质。
 * 先恢复业务状态、报告资源条件，再持续处理记录、升级和 TF 文件请求。
 */
void Storage_Task(void *argument)
{
    (void)argument;
    // 1. 登记当前任务为存储所有者，建立受监督的恢复阶段；失败时报告致命故障并退出。
    if (!storage_runtime_start()) {
        Monitor_ReportFatal(APP_MODULE_STORAGE);
        osThreadExit();
        return;
    }
    storage_upgrade_init(s_flash_ok);
    storage_m2_init();

    // 2. 恢复序列、身份、配置、时间下界和控制日志，尚未对其他模块发布资源就绪。
    // 正常启动允许首次配置；试运行仅恢复，不写空白高水位或时间下界。
    bool bootstrap_write = storage_mutation_begin();
    bool seq_ok = s_flash_ok && storage_seq_init() == HAL_OK;
    bool identity_ok = s_flash_ok && storage_identity_init() == HAL_OK;
    bool config_ok = s_flash_ok && storage_config_check() == HAL_OK;
    s_floor_ok = s_flash_ok && storage_timefloor_init() == HAL_OK;
    s_log_ok = s_flash_ok && storage_ctrllog_init() == HAL_OK;
    if (bootstrap_write) storage_mutation_end();

    // 3. 检查 TF 记录恢复结果及序号一致性，分别得出核心可用性和本轮恢复健康结果。
    HAL_StatusTypeDef tf_check = storage_record_restore();
    /* 序号元数据丢失或倒退时，不能从初始值重新发号覆盖既有记录。 */
    uint64_t high_watermark = storage_seq_get_high_watermark();
    bool sequence_consistent = high_watermark > storage_ctrllog_last_seq() &&
                               high_watermark > storage_record_last_seq();
    bool core_ok = seq_ok && sequence_consistent && identity_ok && config_ok &&
                   s_floor_ok && s_log_ok && storage_m2_settled();
    printf("[M3] restore flash=%u seq=%u id=%u cfg=%u floor=%u log=%u tf=%u\r\n",
           s_flash_ok, seq_ok, identity_ok, config_ok, s_floor_ok, s_log_ok, (unsigned)tf_check);
    // runtime_finish 保存准入/健康状态并向 Monitor 报告进展，不等同于给所有资源置位。
    storage_runtime_finish(core_ok, core_ok && tf_check == HAL_OK);

    // 4. 核心恢复通过后绑定升级请求/响应队列，实际读写仍由本任务消费请求完成。
    if (core_ok) {
        const UpgradeStorageBinding binding = {
            storage_upgrade_req_queue(), storage_upgrade_rsp_queue(), UPGRADE_STORAGE_PROTOCOL_VERSION
        };
        UpgradeStageResult st = UpgradeStorage_Bind(&binding);
        if (st != UPGRADE_STAGE_OK) printf("[UPGQ] bind not ready result=%d\r\n", (int)st);
    }
    storage_upgrade_tf_bind();
    // 当前 STORAGE 和 CONTROL_STATE 两项就绪位都按 core_ok 发布。
    // TF 是否已挂载仍需查询记录服务的实际状态，TIME 则独立依据时间服务判断。
    App_SetResourceReady(APP_RESOURCE_STORAGE, core_ok);
    App_SetResourceReady(APP_RESOURCE_TIME, storage_time_is_valid());
    App_SetResourceReady(APP_RESOURCE_CONTROL_STATE, core_ok);

    uint32_t last_floor_tick = HAL_GetTick() - STORAGE_TIMEFLOOR_STEP_SEC * 1000U;
    uint32_t last_time_check_tick = HAL_GetTick();
    uint32_t last_cd_poll_tick = HAL_GetTick();
    uint32_t last_retry_tick = 0U;
    bool last_card_present = sdcard_physical_present();

    // 5. 进入持续服务阶段；卡检测、时间检查、业务维护和请求队列在同一所有者任务中推进。
    for (;;) {
        if (!storage_runtime_wait(5000U)) {
            Monitor_ReportFatal(APP_MODULE_STORAGE);
            osThreadExit();
            return;
        }

        /* RTC校时或失效后同步更新就绪状态，沿用现有Storage任务和时间服务。 */
        if ((uint32_t)(HAL_GetTick() - last_time_check_tick) >= 1000U) {
            last_time_check_tick = HAL_GetTick();
            App_SetResourceReady(APP_RESOURCE_TIME, storage_time_is_valid());
        }

        bool now_present = sdcard_physical_present();
        bool cd_event = s_cd_event_flag;
        s_cd_event_flag = false;
        if ((uint32_t)(HAL_GetTick() - last_cd_poll_tick) >= 2000U) {
            last_cd_poll_tick = HAL_GetTick();
            cd_event = cd_event || now_present != last_card_present;
        }
        if (cd_event) {
            storage_upgrade_tf_invalidate(false);
            storage_record_unmount();
            storage_upgrade_tf_invalidate(true);
            (void)sdcard_tf_deinit();
            tf_check = now_present ? storage_record_restore() : HAL_OK;
            storage_runtime_finish(core_ok, core_ok && tf_check == HAL_OK);
            if (!storage_runtime_wait(5000U)) {
                Monitor_ReportFatal(APP_MODULE_STORAGE);
                osThreadExit();
                return;
            }
            last_card_present = now_present;
            printf("[M3] TF media changed present=%u check=%u\r\n",
                   now_present, (unsigned)tf_check);
        }

        if (storage_mutation_begin()) {
            if ((uint32_t)(HAL_GetTick() - last_floor_tick) >= STORAGE_TIMEFLOOR_STEP_SEC * 1000U) {
                last_floor_tick = HAL_GetTick();
                uint32_t now = storage_get_unix_time();
                if (now != 0U && storage_timefloor_update(now) != HAL_OK)
                    printf("[M3] time floor update failed\r\n");
            }
            (void)storage_ctrllog_maintenance();
            (void)storage_m2_process();
            storage_record_maintenance();

            /* 只在正常业务阶段挂载/恢复缓存；失败不会触发自动格式化。 */
            if (now_present && tf_check == HAL_OK && !storage_record_is_ready() &&
                (cd_event || (uint32_t)(HAL_GetTick() - last_retry_tick) >= 10000U)) {
                last_retry_tick = HAL_GetTick();
                HAL_StatusTypeDef st = storage_record_sync();
                if (st == HAL_OK) st = storage_record_init();
                storage_upgrade_tf_invalidate(true);
                printf("[M3] TF recovery result=%u ready=%u\r\n",
                       (unsigned)st, storage_record_is_ready());
            }
            storage_mutation_end();
        }

        // 新业务暂停后仍按原顺序处理升级、文件和结算/同步请求，让消费者能完成收尾及 READY 写入。
        // 不能把暂停实现为挂起 Storage_Task，否则升级任务会一直等不到这些服务的真实应答。
        storage_upgrade_q_process(1U);
        storage_upgrade_tf_process();
        storage_runtime_process();
        // 一轮工作后报告真实进展和存储健康，最终是否喂狗由 Monitor 汇总决定。
        storage_runtime_report();
        osDelay(1U);
    }
}
