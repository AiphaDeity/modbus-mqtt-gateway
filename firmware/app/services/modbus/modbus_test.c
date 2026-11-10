/**
 * @file modbus_test.c
 * @brief Modbus 模块单元测试代码
 *
 *  使用方法：
 *    1. 在 CMakeLists.txt 里把本文件加进去（或临时加到工程里）
 *    2. 在 main() 里 Modbus_Init() / vModbusPollTask 创建之后调用 test_add_three_slaves()
 *    3. poll 跑起来后调用 test_read_snapshot() / test_parse_points() 验证
 *
 *  本文件纯测试用，量产时可以整个删除或条件编译排除。
 */

#include "mb_api.h"
#include "mb_pointmap.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 遍历从站时的通用日志回调 */
static int test_dump_slave_cb(mb_slave_runtime_t *s, void *ctx)
{
    (void)ctx;
    MB_LOGI("    id=%d fc=%d addr=%d qty=%d interval=%lu ms enable=%d",
            s->telegram.u8id, s->telegram.u8fct,
            s->telegram.u16RegAdd, s->telegram.u16CoilsNo,
            (unsigned long)s->telegram.collect_interval_ms, s->telegram.enable);
    return 0;
}

/* =========================================================
 * 测试用 1：添加三个从站（线圈 / 保持寄存器 / 输入寄存器）
 * ========================================================= */

void test_add_three_slaves(void)
{
    mb_status_e ret;

    MB_LOGI("====== test_add_three_slaves START ======");


    /* -------------------- 1 号从站：读线圈（FC01） --------------------
     * 地址 0，读 8 位（8 个线圈打包到 reg_data_buf[0] 的低 8 位）
     */
    static modbus_t cfg1 = {
        .u8id               = 1,
        .u8fct              = MB_FC_READ_COILS,       // FC01
        .u16RegAdd          = 0,
        .u16CoilsNo         = 8,                      // 8 个线圈
        .u16reg             = NULL,                   // 让库自己分配 reg_data_buf
        .u32CurrentTask     = NULL,
        .collect_interval_ms = 1000,                  // 1 秒采一次
        .enable             = 1,
        .retry_cnt          = 1,
    };

    // 线圈点位表：8 个线圈 → 全部在 reg[0]，bit_index = 线圈下标
    static mb_point_t points1[MB_POINTS_PER_SLAVE] = {0};
    for (int i = 0; i < 8; i++)
    {
        points1[i].valid       = 1;
        points1[i].reg_index   = 0;                  // 8 个线圈打包在同一个字里
        points1[i].bit_index  = i;                  // 线圈 i = bit i
        points1[i].data_type  = MB_DT_BIT;
        points1[i].scale      = 1.0f;
        points1[i].offset     = 0.0f;
        snprintf(points1[i].name, MB_POINT_NAME_MAX, "coil_%d", i);
        snprintf(points1[i].unit,  MB_POINT_UNIT_MAX,  "");
    }

    ret = mb_apply_slave_config(MB_CFG_OP_UPSERT, 1, &cfg1, points1);
    MB_LOGI("  slave1 (FC01 coils 8bit) ret=%d", ret);


    /* -------------------- 2 号从站：读保持寄存器（FC03） --------------------
     * 地址 0，读 2 个寄存器
     */
    static modbus_t cfg2 = {
        .u8id               = 2,
        .u8fct              = MB_FC_READ_REGISTERS,    // FC03
        .u16RegAdd          = 0,
        .u16CoilsNo         = 2,
        .u16reg             = NULL,
        .u32CurrentTask     = NULL,
        .collect_interval_ms = 2000,
        .enable             = 1,
        .retry_cnt          = 2,
    };

    static mb_point_t points2[MB_POINTS_PER_SLAVE] = {0};
    points2[0].valid       = 1;
    points2[0].reg_index   = 0;
    points2[0].data_type   = MB_DT_UINT16;
    points2[0].scale       = 0.1f;                   // 原始值 × 0.1
    points2[0].offset      = 0.0f;
    strcpy(points2[0].name, "temperature");
    strcpy(points2[0].unit, "C");

    points2[1].valid       = 1;
    points2[1].reg_index   = 1;
    points2[1].data_type   = MB_DT_UINT16;
    points2[1].scale       = 0.01f;
    points2[1].offset      = 0.0f;
    strcpy(points2[1].name, "humidity");
    strcpy(points2[1].unit, "%");

    ret = mb_apply_slave_config(MB_CFG_OP_UPSERT, 2, &cfg2, points2);
    MB_LOGI("  slave2 (FC03 holding regs x2) ret=%d", ret);


    /* -------------------- 3 号从站：读输入寄存器（FC04） --------------------
     * 地址 0，读 2 个寄存器（第二个拼 32 位）
     */
    static modbus_t cfg3 = {
        .u8id               = 3,
        .u8fct              = MB_FC_READ_INPUT_REGISTER,  // FC04
        .u16RegAdd          = 0,
        .u16CoilsNo         = 2,
        .u16reg             = NULL,
        .u32CurrentTask     = NULL,
        .collect_interval_ms = 500,                   // 500ms 快速
        .enable             = 1,
        .retry_cnt          = 0,
    };

    static mb_point_t points3[MB_POINTS_PER_SLAVE] = {0};
    points3[0].valid       = 1;
    points3[0].reg_index   = 0;
    points3[0].data_type   = MB_DT_INT16;             // 有符号
    points3[0].scale       = 1.0f;
    points3[0].offset      = 0.0f;
    strcpy(points3[0].name, "analog_in1");
    strcpy(points3[0].unit, "mV");

    points3[1].valid       = 1;
    points3[1].reg_index   = 0;                       // ★ reg_index=0, 占 reg[0]+reg[1]
    points3[1].data_type   = MB_DT_UINT32;            // 两个寄存器拼 32 位大端
    points3[1].scale       = 1.0f;
    points3[1].offset      = 0.0f;
    strcpy(points3[1].name, "counter");
    strcpy(points3[1].unit, "");

    ret = mb_apply_slave_config(MB_CFG_OP_UPSERT, 3, &cfg3, points3);
    MB_LOGI("  slave3 (FC04 input regs x2) ret=%d", ret);


    /* -------------------- 验证：遍历所有从站 -------------------- */
    MB_LOGI("  --- verify via mb_for_each_valid_slave ---");
    uint8_t cnt = mb_for_each_valid_slave(test_dump_slave_cb, NULL);
    MB_LOGI("  total valid slaves: %d (expect 3)", cnt);

    MB_LOGI("====== test_add_three_slaves END ======");
}


/* =========================================================
 * 测试用 2：读快照（poll 跑起来之后调用）
 * ========================================================= */

void test_read_snapshot(void)
{
    MB_LOGI("====== test_read_snapshot START ======");

    for (uint8_t id = 1; id <= 3; id++)
    {
        mb_slave_snapshot_t snap;
        mb_status_e ret = mb_take_slave_snapshot(id, &snap);
        if (ret != MB_STATUS_OK)
        {
            MB_LOGW("  slave%d not found ret=%d", id, ret);
            continue;
        }

        MB_LOGI("  --- slave %d snapshot ---", id);
        MB_LOGI("    fc=%d online=%d data_valid=%d valid_point=%d status=%d",
                snap.func_code, snap.is_online, snap.data_valid,
                snap.valid_point, snap.collect_status);
        MB_LOGI("    last_collect_tick=%lu", snap.last_collect_tick);

        if (snap.func_code == MB_FC_READ_COILS || snap.func_code == MB_FC_READ_DISCRETE_INPUT)
        {
            // FC01/02 线圈打包字（Modbus 标准 LSB first）
            MB_LOGI("    coils packed: reg[0]=0x%04X", snap.data[0]);
            for (int i = 0; i < 8; i++)
                MB_LOGI("      coil_%d = %d", i, (snap.data[0] >> i) & 1);
        }
        else
        {
            // FC03/04 寄存器原始值
            for (uint16_t i = 0; i < snap.valid_point; i++)
                MB_LOGI("    reg[%d] = %u (0x%04X)", i, snap.data[i], snap.data[i]);
        }
    }

    MB_LOGI("====== test_read_snapshot END ======");
}


/* =========================================================
 * 测试用 3：点位解析（从 RAM points_cache 取，零 Flash 开销）
 * ========================================================= */

void test_parse_points(void)
{
    MB_LOGI("====== test_parse_points START ======");

    for (uint8_t id = 1; id <= 3; id++)
    {
        mb_parsed_point_t parsed[MB_POINTS_PER_SLAVE];
        int cnt = mb_pointmap_parse_slave(id, parsed);
        if (cnt < 0)
        {
            MB_LOGW("  slave%d parse fail (slave not exist or no valid points)", id);
            continue;
        }

        MB_LOGI("  --- slave %d parsed %d points ---", id, cnt);
        for (int i = 0; i < cnt; i++)
        {
            MB_LOGI("    %-16s = %10.2f %s",
                    parsed[i].name, parsed[i].value, parsed[i].unit);
        }
    }

    MB_LOGI("====== test_parse_points END ======");
}

/* =========================================================
 * 测试用 3.5：写操作验证（FC05 写线圈 / FC06 写保持寄存器）
 * ========================================================= */

void test_write_operations(void)
{
    static uint8_t  done      = 0;
    static uint32_t start_tick = 0;

    if (done) return;

    if (start_tick == 0)
        start_tick = osKernelGetTickCount();

    /* 等系统先跑几轮读轮询，再投写请求，便于观察"写抢占读" */
    if ((osKernelGetTickCount() - start_tick) < 10000U)
        return;

    MB_LOGI("====== test_write_operations START ======");

    /* ---------- id=4：写单线圈（FC05），线圈 0 = ON ---------- */
    uint16_t coil_val = 1;   /* 1=ON, 0=OFF；库内自动转 0xFF00/0x0000 */
    mb_status_e r1 = mb_inject_write(4, MB_FC_WRITE_COIL,
                                     0, 1, &coil_val,
                                     NULL, NULL, 0);
    MB_LOGI("  inject FC05 slave4 coil0=ON ret=%d", r1);

    /* ---------- id=5：写单保持寄存器（FC06），地址 0 = 0x1234 ---------- */
    uint16_t reg_val = 0x1234;
    mb_status_e r2 = mb_inject_write(5, MB_FC_WRITE_REGISTER,
                                     0, 1, &reg_val,
                                     NULL, NULL, 0);
    MB_LOGI("  inject FC06 slave5 reg0=0x%04X ret=%d", reg_val, r2);

    done = 1;
    MB_LOGI("====== test_write_operations END (poll will log write OK/FAIL) ======");
}

/* =========================================================
 * 测试用 4：删除 + 再加回来（验证 RAM 状态机正确）
 * ========================================================= */

void test_delete_and_re_add(void)
{
    MB_LOGI("====== test_delete_and_re_add START ======");

    // 删 2 号
    mb_status_e ret = mb_apply_slave_config(MB_CFG_OP_DELETE, 2, NULL, NULL);
    MB_LOGI("  delete slave2 ret=%d", ret);

    // 验证只剩 1、3 号
    uint8_t cnt = mb_for_each_valid_slave(test_dump_slave_cb, NULL);
    MB_LOGI("  after delete: %d slaves (expect 2)", cnt);

    // 把 2 号加回来（参数同 test_add_three_slaves）
    modbus_t cfg2 = {
        .u8id = 2, .u8fct = MB_FC_READ_REGISTERS,
        .u16RegAdd = 0, .u16CoilsNo = 2, .u16reg = NULL,
        .collect_interval_ms = 2000, .enable = 1, .retry_cnt = 2,
    };
    mb_point_t points2[MB_POINTS_PER_SLAVE] = {0};
    points2[0].valid = 1; points2[0].reg_index = 0; points2[0].data_type = MB_DT_UINT16;
    points2[0].scale = 0.1f; points2[0].offset = 0.0f;
    strcpy(points2[0].name, "temperature"); strcpy(points2[0].unit, "C");
    points2[1].valid = 1; points2[1].reg_index = 1; points2[1].data_type = MB_DT_UINT16;
    points2[1].scale = 0.01f; points2[1].offset = 0.0f;
    strcpy(points2[1].name, "humidity"); strcpy(points2[1].unit, "%");

    ret = mb_apply_slave_config(MB_CFG_OP_UPSERT, 2, &cfg2, points2);
    MB_LOGI("  re-add slave2 ret=%d", ret);

    cnt = mb_for_each_valid_slave(test_dump_slave_cb, NULL);
    MB_LOGI("  after re-add: %d slaves (expect 3)", cnt);

    MB_LOGI("====== test_delete_and_re_add END ======");
}


/* =========================================================
 * 测试用 5：批量配置开始/结束（模拟上电加载）
 * ========================================================= */

void test_batch_config(void)
{
    MB_LOGI("====== test_batch_config START ======");

    // 先把现有三个都删掉
    for (uint8_t id = 1; id <= 3; id++)
        mb_apply_slave_config(MB_CFG_OP_DELETE, id, NULL, NULL);

    uint8_t cnt = mb_for_each_valid_slave(test_dump_slave_cb, NULL);
    MB_LOGI("  before batch: %d slaves (expect 0)", cnt);

    // 批量添加：抑制中间回调
    mb_config_batch_begin();

    // （这里三个从站的配置抄 test_add_three_slaves 里的）
    modbus_t cfg1 = { .u8id=1, .u8fct=MB_FC_READ_COILS, .u16RegAdd=0,
                      .u16CoilsNo=8, .u16reg=NULL, .collect_interval_ms=1000,
                      .enable=1, .retry_cnt=1 };
    mb_point_t p1[MB_POINTS_PER_SLAVE] = {0};
    for (int i = 0; i < 8; i++) {
        p1[i].valid=1; p1[i].reg_index=0; p1[i].bit_index=i;
        p1[i].data_type=MB_DT_BIT; p1[i].scale=1.0f;
        snprintf(p1[i].name, MB_POINT_NAME_MAX, "coil_%d", i);
    }
    mb_apply_slave_config(MB_CFG_OP_UPSERT, 1, &cfg1, p1);

    modbus_t cfg2 = { .u8id=2, .u8fct=MB_FC_READ_REGISTERS, .u16RegAdd=0,
                      .u16CoilsNo=2, .u16reg=NULL, .collect_interval_ms=2000,
                      .enable=1, .retry_cnt=2 };
    mb_point_t p2[MB_POINTS_PER_SLAVE] = {0};
    p2[0].valid=1; p2[0].reg_index=0; p2[0].data_type=MB_DT_UINT16;
    p2[0].scale=0.1f; strcpy(p2[0].name, "temperature"); strcpy(p2[0].unit, "C");
    p2[1].valid=1; p2[1].reg_index=1; p2[1].data_type=MB_DT_UINT16;
    p2[1].scale=0.01f; strcpy(p2[1].name, "humidity"); strcpy(p2[1].unit, "%");
    mb_apply_slave_config(MB_CFG_OP_UPSERT, 2, &cfg2, p2);

    modbus_t cfg3 = { .u8id=3, .u8fct=MB_FC_READ_INPUT_REGISTER, .u16RegAdd=0,
                      .u16CoilsNo=2, .u16reg=NULL, .collect_interval_ms=500,
                      .enable=1, .retry_cnt=0 };
    mb_point_t p3[MB_POINTS_PER_SLAVE] = {0};
    p3[0].valid=1; p3[0].reg_index=0; p3[0].data_type=MB_DT_INT16;
    strcpy(p3[0].name, "analog_in1"); strcpy(p3[0].unit, "mV");
    p3[1].valid=1; p3[1].reg_index=0; p3[1].data_type=MB_DT_UINT32;
    strcpy(p3[1].name, "counter");
    mb_apply_slave_config(MB_CFG_OP_UPSERT, 3, &cfg3, p3);

    // 结束批量：触发一次 GLOBAL 回调（points_cache 在 batch_begin 期间也照样更新）
    mb_config_batch_end();

    cnt = mb_for_each_valid_slave(test_dump_slave_cb, NULL);
    MB_LOGI("  after batch: %d slaves (expect 3)", cnt);

    MB_LOGI("====== test_batch_config END ======");
}


/* =========================================================
 * 一键全跑（放到某个任务里周期调，或者 main 里串行调）
 * ========================================================= */

void test_modbus_all(void)
{
    test_add_three_slaves();

    // 延时等 poll 至少跑一轮（没有硬件就等一会超时也行）
    osDelay(pdMS_TO_TICKS(2000));

    test_read_snapshot();
    test_parse_points();

    test_delete_and_re_add();
    test_batch_config();

    MB_LOGI("====== ALL TESTS DONE ======");
}
