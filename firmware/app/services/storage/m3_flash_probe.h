#ifndef M3_FLASH_PROBE_H
#define M3_FLASH_PROBE_H

/*
 * W25Q128 读不到 JEDEC 时的分层探测（排障专用，默认不参与编译）。
 *
 * 为什么单独成文件：
 *   这套探测有 200 行，只在上板排查时用得上。如果和生产代码混在
 *   m3_selftest.c 里，正常读代码时会被大段 .code 打断。拆出来后
 *   生产固件里根本没有这段代码，不占 FLASH、不占串口时间。
 *
 * 什么时候打开：
 *   CMake 配置项 M3_ENABLE_FLASH_PROBE 默认 OFF。
 *   需要排查时重新 Configure 并打开：
 *       cmake -DM3_ENABLE_FLASH_PROBE=ON <其它原有参数>
 *   Visual Studio Code 的 CMake Tools 里也可以在
 *   「CMake: Edit Cache」中把 M3_ENABLE_FLASH_PROBE 改成 ON，
 *   然后必须再执行一次「CMake: Configure」才会生效。
 *
 * 一次上电打印六段：
 *   D0  SPI2 外设句柄状态（State / ErrorCode / 时钟参数）
 *   D1  PCA9555 三个寄存器，含 8 路拨码的实际电平 → 拨码到底生效没有
 *   D2  0xAB 释放深掉电后的 SR1 → 芯片是否在 DPD 状态
 *   D3  单窗口读 JEDEC → 排除"命令与接收分开两次 HAL 调用"的时序错位
 *   D4  软件强制 BIT3=HIGH → 读 JEDEC（用来判定哪个极性是 SPI 侧）
 *   D5  软件强制 BIT3=LOW  → 读 JEDEC
 */
void m3_flash_probe_run(void);

#endif /* M3_FLASH_PROBE_H */
