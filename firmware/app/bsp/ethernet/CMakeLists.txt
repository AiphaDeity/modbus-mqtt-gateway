# M2 以太网 BSP 模块：LAN8720A + LwIP 带 OS 移植层（Day 2）
# 源文件：ethernet.c（业务层）+ ethernetif.c（网卡驱动）+ sys_arch.c（搬运的 FreeRTOS 适配）
add_library(m2_ethernet STATIC
    ethernet.c
    ethernetif.c
    sys_arch.c        # 从探索者例程 arch/sys_arch.c 原样拷入（FreeRTOS 适配层，一行不改）
)
target_include_directories(m2_ethernet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# stm32cubemx：HAL/FreeRTOS/Core(FreeRTOSConfig.h) 的 include
# lwip：协议栈头文件 + lwipopts.h（lwip 库 PUBLIC 传递 bsp/ethernet 目录）
target_link_libraries(m2_ethernet PUBLIC stm32cubemx lwip)
