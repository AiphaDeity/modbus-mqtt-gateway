target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mqtt_client.c
    ${CMAKE_CURRENT_SOURCE_DIR}/telemetry_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/hmac_sha256.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
