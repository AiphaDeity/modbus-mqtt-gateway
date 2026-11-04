# Day2: 以太网 BSP 子模块（LAN8720A + LwIP 移植层）
add_subdirectory(ethernet)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/rs485/bsp_rs485.c
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/rs485
)