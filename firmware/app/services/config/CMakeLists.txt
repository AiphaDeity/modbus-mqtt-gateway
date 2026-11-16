# Config 模块 CMakeLists
# 配置维护模块：运行时从站配置变更入口

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/config_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mini_json.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
