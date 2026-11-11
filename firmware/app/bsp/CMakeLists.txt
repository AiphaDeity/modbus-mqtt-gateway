# bsp/CMakeLists.txt

# storage（M3 使用）
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/storage
)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/storage/rtc_sd3078.c
    ${CMAKE_CURRENT_LIST_DIR}/storage/spi_flash_w25q128.c
    ${CMAKE_CURRENT_LIST_DIR}/storage/sdcard_tf.c
)

# 在HAL单元预包含现有接口，保留设备超时配置，避免改写生成器或第三方源码。
get_filename_component(GATEWAY_APP_DRIVER_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../Drivers/STM32F4xx_HAL_Driver/Src" ABSOLUTE)
set_source_files_properties(
    ${GATEWAY_APP_DRIVER_DIR}/stm32f4xx_hal_sd.c
    ${GATEWAY_APP_DRIVER_DIR}/stm32f4xx_ll_sdmmc.c
    TARGET_DIRECTORY STM32_Drivers
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/storage/sdcard_tf.h"
)
set_source_files_properties(
    ${GATEWAY_APP_DRIVER_DIR}/stm32f4xx_hal_eth.c
    TARGET_DIRECTORY STM32_Drivers
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/../Core/Inc/eth.h"
)

# 以太网头文件路径始终保留（main.c 会 include ethernet.h），库本身按 M2 开关决定是否编译
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/ethernet
)

# Day2: 以太网 BSP 子模块（LAN8720A + LwIP 移植层）
if(GATEWAY_ENABLE_M2)
    add_subdirectory(ethernet)
endif()
