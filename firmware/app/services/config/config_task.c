/**
 * @file config_task.c
 * @brief 配置维护模块实现 —— 运行时从站配置变更入口
 *
 *  本文件实现 Config_Init（上电加载）和 Config_Task（运行时变更）。
 *
 *  Config_Init 三级降级加载：
 *    1. TF 卡 /config.json 存在 → f_open 读文件 → MiniJson 解析 → mb_apply_slave_config
 *    2. Flash 有有效配置(storage_config_has_valid) → storage_config_open/read_chunk → MiniJson 解析 → mb_apply_slave_config
 *    3. 都没有 → mb_fixed_points_apply（编译期固定4站）
 *
 *  Config_HandleMessage 处理 MQTT 下发：
 *    解析 {"op":"upsert","slaves":[...]} → 逐站调 mb_apply_slave_config(UPSERT)
 *    解析 {"op":"delete","slaves":[{"id":1}]} → 逐站调 mb_apply_slave_config(DELETE)
 *
 *  数据类型字符串映射：
 *    "uint16"  → MB_DT_UINT16   "int16" → MB_DT_INT16
 *    "uint32"  → MB_DT_UINT32   "int32" → MB_DT_INT32
 *    "float32" → MB_DT_FLOAT32  "bit"   → MB_DT_BIT
 */
#include "config_task.h"           // 本文件对应头文件

#include <string.h>                // memcpy, strlen, strcmp, strncmp
#include <stdio.h>                 // snprintf, printf
#include <stdbool.h>               // bool 类型

#include "FreeRTOS.h"              // FreeRTOS 总头文件
#include "task.h"                  // FreeRTOS 任务相关
#include "cmsis_os2.h"             // CMSIS-RTOS2 封装层

#include "mini_json.h"             // 精简 JSON 解析器
#include "mb_api.h"                // M1 Modbus 主接口：mb_apply_slave_config 等
#include "mb_fixed_points.h"       // 编译期固定点表（降级默认）
#include "mb_pointmap.h"           // 点位表结构和数据类型枚举
#include "modbus.h"                // modbus_t 结构体定义
#include "app_config.h"            // 全局配置结构体
#include "upgrade_quiesce.h"       // OTA 升级静默期门控

#if GATEWAY_ENABLE_M2
#include "storage_config.h"        // M3 Flash 配置存储接口
#include "fatfs.h"                 // FatFS 文件系统接口
#include "ff.h"                    // FatFS 类型定义（FIL, FRESULT 等）
#include "sdcard_tf.h"             // TF 卡 SDIO 驱动
#include "app_monitor.h"           // 应用监控
#endif

/* ===== 配置常量 ===== */

// TF 卡配置文件路径（根目录下的 config.json）
#define CONFIG_FILE_PATH       "/config.json"

// 单条配置 JSON 最大长度（4KB，足够容纳 16 站 × 16 点的完整配置）
#define CONFIG_JSON_MAX        4096U

// 配置任务栈里用的 JSON 缓冲区，放在 CCMRAM 省 SRAM
// static 让它只在本文件可见
static char s_json_buf[CONFIG_JSON_MAX] __attribute__((section(".bss.ccmram")));

/* ===== 内部：字符串转数据类型枚举 ===== */
// 把 JSON 里的 type 字段字符串转成 M1 的 mb_data_type_t 枚举
// type_str 数据类型字符串，如 "uint32"
// 返回对应的枚举值；无法识别返回 MB_DT_UINT16（安全默认值）
static mb_data_type_t parse_data_type(const char *type_str)
{
    if (!type_str) return MB_DT_UINT16;       // 空指针，返回安全默认值

    // 逐个比较已知的类型字符串
    if (strcmp(type_str, "uint16")  == 0) return MB_DT_UINT16;    // 无符号16位
    if (strcmp(type_str, "int16")   == 0) return MB_DT_INT16;     // 有符号16位
    if (strcmp(type_str, "uint32")  == 0) return MB_DT_UINT32;    // 无符号32位
    if (strcmp(type_str, "int32")   == 0) return MB_DT_INT32;     // 有符号32位
    if (strcmp(type_str, "float32") == 0) return MB_DT_FLOAT32;   // 32位浮点
    if (strcmp(type_str, "bit")     == 0) return MB_DT_BIT;       // 位提取

    // 未知类型字符串，返回安全默认值并打印警告
    printf("[CFG] unknown type '%s', default uint16\r\n", type_str);  // 打印警告
    return MB_DT_UINT16;                      // 返回安全默认值
}

/* ===== 内部：从 JSON 节点解析一个点位到 mb_point_t ===== */
// slave_pts 是 JSON 数组的一个元素，包含 reg_index/type/scale/offset/name/unit/bit_index
// out_point 输出参数，解析结果填到这里
// 返回 true=成功；false=解析失败
static bool parse_one_point(const mini_json_t *slave_pts, mb_point_t *out_point)
{
    // 先把整个点位结构体清零，确保空位 valid=0
    memset(out_point, 0, sizeof(mb_point_t));

    // reg_index：在 reg_data_buf 中的起始下标，默认0
    out_point->reg_index = (uint16_t)MiniJson_GetInt(slave_pts, "reg_index", 0);

    // type：数据类型字符串转枚举
    const char *type_str = MiniJson_GetString(slave_pts, "type", "uint16");  // 默认 uint16
    out_point->data_type = parse_data_type(type_str);                        // 字符串转枚举

    // scale：系数，raw * scale，默认 1.0（原值直传）
    out_point->scale = (float)MiniJson_GetFloat(slave_pts, "scale", 1.0);

    // offset：偏移，raw * scale + offset，默认 0
    out_point->offset = (float)MiniJson_GetFloat(slave_pts, "offset", 0.0);

    // name：点位名称（≤7字符，MB_POINT_NAME_MAX=8 含\0）
    const char *name = MiniJson_GetString(slave_pts, "name", "");
    // strncpy 会自动在末尾补\0，但为防止源串过长不截断，手动截断
    strncpy(out_point->name, name, MB_POINT_NAME_MAX - 1);   // 最多拷贝 7 字符
    out_point->name[MB_POINT_NAME_MAX - 1] = '\0';           // 强制结尾\0

    // unit：单位（≤7字符）
    const char *unit = MiniJson_GetString(slave_pts, "unit", "");
    strncpy(out_point->unit, unit, MB_POINT_UNIT_MAX - 1);   // 最多拷贝 7 字符
    out_point->unit[MB_POINT_UNIT_MAX - 1] = '\0';           // 强制结尾\0

    // bit_index：仅 type=bit 时用，默认0
    out_point->bit_index = (uint8_t)MiniJson_GetInt(slave_pts, "bit_index", 0);

    // 标记为有效配置
    out_point->valid = 1;

    return true;                              // 解析成功
}

/* ===== 内部：从 JSON 节点解析一个从站并调 mb_apply_slave_config ===== */
// slave_node 是 slaves 数组的一个元素，包含 id/fc/reg/count/interval/enable/retry/points
// 返回 0=成功；-1=解析失败；-2=M1 拒绝（槽位满或参数非法）
static int apply_one_slave(const mini_json_t *slave_node)
{
    if (!slave_node) return -1;               // 空指针，失败

    // 从站地址：1~247，默认0表示无效
    int id = MiniJson_GetInt(slave_node, "id", 0);
    if (id < 1 || id > 247)                   // 地址范围校验
    {
        printf("[CFG] invalid slave id %d\r\n", id);  // 打印错误
        return -1;                            // 地址非法，失败
    }

    // 功能码：1~4，默认3（读保持寄存器，最常用）
    int fc = MiniJson_GetInt(slave_node, "fc", 3);
    if (fc < 1 || fc > 4)                     // 功能码范围校验
    {
        printf("[CFG] invalid fc %d, default 3\r\n", fc);  // 打印警告
        fc = 3;                               // 修正为默认值
    }

    // 寄存器起始地址，默认0
    int reg = MiniJson_GetInt(slave_node, "reg", 0);

    // 寄存器数量，默认1
    int count = MiniJson_GetInt(slave_node, "count", 1);
    if (count < 1 || count > 32)              // 数量范围校验（MB_REG_MAX_PER_SLAVE=32）
    {
        printf("[CFG] invalid count %d, clamp to 1\r\n", count);  // 打印警告
        count = 1;                            // 修正为最小值
    }

    // 采集周期(ms)，默认2000（2秒）
    int interval = MiniJson_GetInt(slave_node, "interval", 2000);

    // 是否启用，默认true
    bool enable = MiniJson_GetBool(slave_node, "enable", true);

    // 重试次数，默认1
    int retry = MiniJson_GetInt(slave_node, "retry", 1);

    // 填充 modbus_t 配置结构体（M1 的从站协议参数）
    modbus_t cfg;
    memset(&cfg, 0, sizeof(cfg));             // 先清零
    cfg.u8id               = (uint8_t)id;     // 从站地址
    cfg.u8fct              = (mb_functioncode_t)fc;  // 功能码
    cfg.u16RegAdd          = (uint16_t)reg;   // 寄存器起始地址
    cfg.u16CoilsNo         = (uint16_t)count; // 寄存器数量
    cfg.u16reg             = NULL;            // 运行期内部用 reg_data_buf，这里填 NULL
    cfg.u32CurrentTask     = NULL;            // 不使用任务通知
    cfg.collect_interval_ms = (uint32_t)interval;  // 采集周期
    cfg.enable             = enable ? 1 : 0;  // 启用标志
    cfg.retry_cnt          = (uint8_t)retry;  // 重试次数

    // 解析点位表（points 数组）
    // 先把点位数组清零，空位 valid=0
    mb_point_t points[MB_POINTS_PER_SLAVE];
    memset(points, 0, sizeof(points));        // 全部清零，所有点位 valid=0

    // 从 JSON 中取 points 数组
    mini_json_t *pts_arr = MiniJson_ObjectGet(slave_node, "points");  // 取 points 字段
    int pts_count = 0;                        // 已解析的点位数

    if (pts_arr && pts_arr->type == MJ_TYPE_ARRAY)  // points 字段存在且是数组
    {
        int arr_size = MiniJson_ArraySize(pts_arr);  // 取数组元素个数
        for (int i = 0; i < arr_size && i < MB_POINTS_PER_SLAVE; i++)  // 遍历，不超过上限
        {
            mini_json_t *pt_node = MiniJson_ArrayGet(pts_arr, i);  // 取第 i 个点位
            if (pt_node)                      // 节点存在
            {
                parse_one_point(pt_node, &points[i]);  // 解析这个点位
                pts_count++;                  // 计数
            }
        }
    }

    printf("[CFG] apply slave id=%d fc=%d reg=%d count=%d pts=%d\r\n",
           id, fc, reg, count, pts_count);    // 打印解析结果，方便调试

    // 调 M1 统一入口注册从站 + 点位表
    // UPSERT = 新增或更新；cfg 非 NULL 注册从站协议，points 非空填点位表
    mb_status_e st = mb_apply_slave_config(MB_CFG_OP_UPSERT,
                                            (uint8_t)id,
                                            &cfg,
                                            points);
    if (st != MB_STATUS_OK)                   // M1 返回失败
    {
        printf("[CFG] mb_apply_slave_config failed slave=%d st=%d\r\n", id, (int)st);
        return -2;                            // M1 拒绝
    }

    return 0;                                 // 成功
}

/* ===== 内部：从 JSON 节点删除一个从站 ===== */
// slave_node 是 slaves 数组的一个元素，只取 id 字段
// 返回 0=成功；-1=解析失败；-2=M1 拒绝
static int delete_one_slave(const mini_json_t *slave_node)
{
    if (!slave_node) return -1;               // 空指针，失败

    // 从站地址
    int id = MiniJson_GetInt(slave_node, "id", 0);
    if (id < 1 || id > 247)                   // 地址范围校验
    {
        printf("[CFG] delete: invalid slave id %d\r\n", id);  // 打印错误
        return -1;                            // 地址非法，失败
    }

    printf("[CFG] delete slave id=%d\r\n", id);  // 打印操作信息

    // 调 M1 统一入口删除从站
    // DELETE = 按 slave_id 删除；cfg 和 points 都传 NULL
    mb_status_e st = mb_apply_slave_config(MB_CFG_OP_DELETE,
                                            (uint8_t)id,
                                            NULL,
                                            NULL);
    if (st != MB_STATUS_OK)                   // M1 返回失败
    {
        printf("[CFG] delete failed slave=%d st=%d\r\n", id, (int)st);
        return -2;                            // M1 拒绝
    }

    return 0;                                 // 成功
}

/* ===== 内部：解析完整配置 JSON 并应用到 M1 ===== */
// json_str 配置变更 JSON 字符串
// 返回 0=成功；-1=JSON解析失败；-2=升级静默期拒绝；-3=部分从站应用失败
static int config_apply_json(const char *json_str)
{
    if (!json_str || !*json_str) return -1;   // 空指针或空字符串，失败

    // 升级静默期检查：OTA 升级期间不允许改从站配置
    if (!UpgradeQuiesce_AllowsMutation())
    {
        printf("[CFG] rejected: upgrade quiesce\r\n");  // 打印拒绝原因
        return -2;                            // 升级静默期，拒绝
    }

    // 用 MiniJson 解析 JSON 字符串
    mini_json_t *root = MiniJson_Parse(json_str);
    if (!root)                                // JSON 解析失败
    {
        printf("[CFG] json parse failed\r\n");  // 打印失败
        return -1;                            // 解析失败
    }

    // 取操作类型：upsert 或 delete，默认 upsert
    const char *op = MiniJson_GetString(root, "op", "upsert");

    // 取 slaves 数组
    mini_json_t *slaves = MiniJson_ObjectGet(root, "slaves");
    if (!slaves || slaves->type != MJ_TYPE_ARRAY)  // 没有 slaves 数组
    {
        printf("[CFG] no slaves array\r\n");   // 打印错误
        MiniJson_Free(root);                  // 释放 JSON 树
        return -1;                            // 格式错误
    }

    int arr_size = MiniJson_ArraySize(slaves);  // 从站数量
    int ok_count = 0;                         // 成功处理的从站数
    int fail_count = 0;                       // 失败的从站数

    // 批量操作：抑制中间回调，只触发一次 RELOAD
    mb_config_batch_begin();

    // 判断操作类型
    if (strcmp(op, "delete") == 0)            // 删除操作
    {
        // 遍历 slaves 数组，逐个删除
        for (int i = 0; i < arr_size; i++)
        {
            mini_json_t *s = MiniJson_ArrayGet(slaves, i);  // 取第 i 个从站
            int r = delete_one_slave(s);      // 删除
            if (r == 0) ok_count++; else fail_count++;  // 统计成功失败
        }
    }
    else                                      // upsert 操作（新增或更新）
    {
        // 遍历 slaves 数组，逐个新增/更新
        for (int i = 0; i < arr_size; i++)
        {
            mini_json_t *s = MiniJson_ArrayGet(slaves, i);  // 取第 i 个从站
            int r = apply_one_slave(s);       // 新增/更新
            if (r == 0) ok_count++; else fail_count++;  // 统计成功失败
        }
    }

    // 批量结束：触发一次 RELOAD 回调（存储模块会收到通知，写一次 Flash）
    mb_config_batch_end();

    printf("[CFG] done: op=%s ok=%d fail=%d\r\n", op, ok_count, fail_count);  // 打印汇总

    MiniJson_Free(root);                      // 释放 JSON 树

    if (fail_count > 0) return -3;            // 有失败，返回部分失败
    return 0;                                 // 全部成功
}

#if GATEWAY_ENABLE_M2
/* ===== 内部：从 TF 卡读取 config.json ===== */
// 读取 /config.json 到 s_json_buf
// 返回 true=成功；false=文件不存在或读取失败
static bool config_load_from_tf(void)
{
    // 先检查 TF 卡是否就绪
    if (!sdcard_tf_is_ready())
    {
        printf("[CFG] TF card not ready\r\n");  // 打印信息
        return false;                         // TF 卡不可用
    }

    // 挂载 FatFS 文件系统
    FRESULT fr = f_mount(&SDFatFS, SDPath, 1);  // 1=立即挂载
    if (fr != FR_OK)
    {
        printf("[CFG] f_mount failed: %d\r\n", (int)fr);  // 打印错误
        return false;                         // 挂载失败
    }

    // 打开配置文件
    FIL f;                                    // FatFS 文件对象
    fr = f_open(&f, CONFIG_FILE_PATH, FA_READ);  // 只读打开
    if (fr != FR_OK)
    {
        printf("[CFG] no %s on TF\r\n", CONFIG_FILE_PATH);  // 文件不存在（正常情况）
        f_mount(NULL, SDPath, 0);             // 卸载文件系统
        return false;                         // 文件不存在
    }

    // 检查文件大小，超过缓冲区上限就拒绝
    if (f.obj.objsize >= CONFIG_JSON_MAX)
    {
        printf("[CFG] config.json too large: %lu\r\n", (unsigned long)f.obj.objsize);
        f_close(&f);                          // 关闭文件
        f_mount(NULL, SDPath, 0);             // 卸载
        return false;                         // 文件太大
    }

    // 读取文件内容到 s_json_buf
    UINT br = 0;                              // 实际读取字节数
    fr = f_read(&f, s_json_buf, CONFIG_JSON_MAX - 1, &br);  // 读取
    f_close(&f);                              // 关闭文件
    f_mount(NULL, SDPath, 0);                 // 卸载文件系统

    if (fr != FR_OK || br == 0)               // 读取失败或空文件
    {
        printf("[CFG] f_read failed: fr=%d br=%u\r\n", (int)fr, (unsigned)br);
        return false;                         // 读取失败
    }

    s_json_buf[br] = '\0';                    // 强制字符串结尾
    printf("[CFG] loaded config.json from TF: %u bytes\r\n", (unsigned)br);  // 打印成功
    return true;                              // 成功读取
}

/* ===== 内部：从 Flash 读取配置 JSON ===== */
// 通过 storage_config 接口从 W25Q128 Flash 读配置 JSON 到 s_json_buf
// 返回 true=成功；false=Flash 无配置或读取失败
static bool config_load_from_flash(void)
{
    // 检查 Flash 里有没有有效配置
    if (!storage_config_has_valid())
    {
        printf("[CFG] no config in Flash\r\n");  // 打印信息
        return false;                         // Flash 无配置
    }

    // 打开配置读取句柄
    storage_config_reader_t rd;               // 配置读取句柄
    if (storage_config_open(&rd) != HAL_OK)
    {
        printf("[CFG] storage_config_open failed\r\n");  // 打印错误
        return false;                         // 打开失败
    }

    // 检查配置长度
    if (rd.total_len == 0 || rd.total_len >= CONFIG_JSON_MAX)
    {
        printf("[CFG] Flash config invalid len: %lu\r\n", (unsigned long)rd.total_len);
        return false;                         // 长度异常
    }

    // 分块读取配置 JSON
    // 每次读 256 字节，直到读完 total_len
    uint32_t offset = 0;                      // 当前读取偏移
    while (offset < rd.total_len)
    {
        // 本次要读的字节数：剩余量和 256 取最小
        uint32_t chunk = rd.total_len - offset;
        if (chunk > 256U) chunk = 256U;       // 每次最多 256 字节

        if (storage_config_read_chunk(&rd, offset,
                                       (uint8_t *)s_json_buf + offset,
                                       chunk) != HAL_OK)
        {
            printf("[CFG] read_chunk failed at offset=%lu\r\n", (unsigned long)offset);
            return false;                     // 读取失败
        }
        offset += chunk;                      // 偏移前移
    }

    s_json_buf[rd.total_len] = '\0';          // 强制字符串结尾
    printf("[CFG] loaded config from Flash: %lu bytes\r\n", (unsigned long)rd.total_len);
    return true;                              // 成功读取
}
#endif /* GATEWAY_ENABLE_M2 */

/* ===== 对外：Config_Init —— 上电加载配置 ===== */
AppModuleInitResult Config_Init(void)
{
    printf("[CFG] Config_Init start\r\n");    // 打印开始日志

#if GATEWAY_ENABLE_M2
    // 第一级：尝试从 TF 卡加载 config.json
    if (config_load_from_tf())
    {
        // TF 卡有配置文件，解析并应用
        int r = config_apply_json(s_json_buf);
        if (r == 0)
        {
            printf("[CFG] loaded from TF card OK\r\n");  // 打印成功
            return APP_MODULE_INIT_OK;        // 成功，返回
        }
        printf("[CFG] TF config apply failed (%d), trying Flash\r\n", r);  // TF 配置应用失败
    }

    // 第二级：尝试从 Flash 加载配置
    if (config_load_from_flash())
    {
        // Flash 有配置，解析并应用
        int r = config_apply_json(s_json_buf);
        if (r == 0)
        {
            printf("[CFG] loaded from Flash OK\r\n");  // 打印成功
            return APP_MODULE_INIT_OK;        // 成功，返回
        }
        printf("[CFG] Flash config apply failed (%d), fallback to fixed\r\n", r);  // Flash 配置失败
    }
#endif

    // 第三级：降级到编译期固定点表
    printf("[CFG] fallback to fixed points\r\n");  // 打印降级信息
    if (mb_fixed_points_apply())              // 应用编译期固定4站
    {
        printf("[CFG] fixed points applied OK\r\n");  // 打印成功
        return APP_MODULE_INIT_OK;            // 成功
    }

    printf("[CFG] ERROR: fixed points failed!\r\n");  // 打印错误
    return APP_MODULE_INIT_FAILED;            // 全部失败
}

/* ===== 对外：Config_Task —— 运行时配置维护任务 ===== */
void Config_Task(void *argument)
{
    (void)argument;                           // 未使用任务参数

    printf("[CFG] Config_Task started\r\n");  // 打印启动日志

    // 注册健康监控：每 10 秒喂一次狗，60 秒没喂判定卡死
    // 配置任务平时很闲（只在收到配置变更命令时才忙），所以喂狗间隔设长一点
    if (GATEWAY_ENABLE_M2 && !Monitor_RegisterHealth(APP_MODULE_CONFIG, 10000U, 60000U))
    {
        printf("[CFG] health register failed\r\n");  // 注册失败
        Monitor_ReportFatal(APP_MODULE_CONFIG);      // 上报致命错误
        osThreadExit();                      // 退出任务
    }

    // 等系统初始化完毕（Modbus 等模块就绪）再开始
    osDelay(pdMS_TO_TICKS(3000U));           // 等 3 秒

    // 配置任务的主循环
    // 目前配置变更通过 Config_HandleMessage 直接调用（M2 的 mqtt_handle_command 同步调用）
    // 所以本任务主要做健康监控和周期性检查（后续可扩展为消息队列异步处理）
    for (;;)                                  // 任务死循环
    {
        // 喂狗
        if (GATEWAY_ENABLE_M2)
        {
            Monitor_ReportProgress(APP_MODULE_CONFIG, true);
        }

        // 每隔 30 秒醒一次检查（目前没有异步消息队列，后续可扩展）
        // 如果后续改成消息队列模式，这里改成 osMessageQueueGet 阻塞等待
        osDelay(pdMS_TO_TICKS(30000U));      // 睡 30 秒
    }
    // 不会到达这里
}

/* ===== 对外：Config_HandleMessage —— 处理 MQTT 下发的配置变更 ===== */
int Config_HandleMessage(const char *json_buf)
{
    // 调内部函数解析 JSON 并应用
    return config_apply_json(json_buf);
}
