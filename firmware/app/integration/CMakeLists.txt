target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/app_entry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_modules.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_readiness.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/app_config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_upgrade.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_monitor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/upgrade_mailbox.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/watchdog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/maint_frame.c
)
if(GATEWAY_ENABLE_M2)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mqtt_client.c
        ${CMAKE_CURRENT_SOURCE_DIR}/telemetry_task.c
        ${CMAKE_CURRENT_SOURCE_DIR}/hmac_sha256.c
    )
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)
