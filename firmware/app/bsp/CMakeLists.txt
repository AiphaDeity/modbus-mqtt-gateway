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

# 以太网头文件路径始终保留（main.c 会 include ethernet.h），库本身按 M2 开关决定是否编译
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/ethernet
)

# Day2: 以太网 BSP 子模块（LAN8720A + LwIP 移植层）
if(GATEWAY_ENABLE_M2)
    add_subdirectory(ethernet)
endif()
