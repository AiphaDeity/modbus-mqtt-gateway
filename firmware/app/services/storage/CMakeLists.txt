# services/storage/CMakeLists.txt
#
# M3_ENABLE_FLASH_PROBE：W25Q128 分层探测（m3_flash_probe.c）的编译开关。
#   默认 OFF —— 生产固件里根本没有这段排障代码，不占 FLASH、不占串口时间。
#   需要排查"读不到 JEDEC"时，重新 Configure 并打开：
#       cmake -DM3_ENABLE_FLASH_PROBE=ON <其它原有参数>
#   打开后上电会先打印 D0~D5 六段探测，再跑常规自检。
option(M3_ENABLE_FLASH_PROBE "编译 W25Q128 分层探测（排障专用）" OFF)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/storage.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_runtime.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_time.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_config.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_seq.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_identity.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_timefloor.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_ctrllog.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_record.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_m2.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_upgrade.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_upgrade_q.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_upgrade_tf.c
    ${CMAKE_CURRENT_LIST_DIR}/m3_selftest.c
)

if(M3_ENABLE_FLASH_PROBE)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/m3_flash_probe.c
    )
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE M3_ENABLE_FLASH_PROBE=1)
else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE M3_ENABLE_FLASH_PROBE=0)
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
