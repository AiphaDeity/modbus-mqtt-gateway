# services/modbus/CMakeLists.txt
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/modbus.c
    ${CMAKE_CURRENT_LIST_DIR}/modbus_proto.c
    ${CMAKE_CURRENT_LIST_DIR}/modbus_app.c
    ${CMAKE_CURRENT_LIST_DIR}/cJSON.c   
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    USE_FREERTOS
)

# 给顶层目标追加头文件搜索路径
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)