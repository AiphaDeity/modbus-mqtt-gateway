target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/app_entry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_modules.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_readiness.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/app_config.c
)
if(GATEWAY_ENABLE_M2)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mqtt_client.c
        ${CMAKE_CURRENT_SOURCE_DIR}/telemetry_task.c
    )
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
