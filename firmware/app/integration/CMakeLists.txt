target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/app_entry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_modules.c
    ${CMAKE_CURRENT_SOURCE_DIR}/app_readiness.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
