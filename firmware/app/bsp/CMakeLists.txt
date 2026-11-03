# bsp/CMakeLists.txt

# rs485（M1 使用）
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/rs485/bsp_rs485.c
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/rs485
)

# storage（M3 使用）— 头文件先接入，.c 实现逐个补
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/storage
)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/storage/rtc_sd3078.c
    ${CMAKE_CURRENT_LIST_DIR}/storage/spi_flash_w25q128.c
    ${CMAKE_CURRENT_LIST_DIR}/storage/sdcard_tf.c
)
