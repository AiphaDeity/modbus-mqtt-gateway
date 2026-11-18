set(MODBUS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mb_poll.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mb_slave.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mb_api.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mb_pointmap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mb_runtime.c
)

add_library(modbus STATIC ${MODBUS_SRCS})

# ★UARTCallback.c 必须直接编进可执行文件，不能放进 libmodbus.a！
#
# 这个文件里只有 HAL_UART_xxxCallback 的强符号覆盖，HAL 驱动里已有同名 __weak
# 空桩。链接器扫描静态库时，只要符号"已被弱定义满足"就不会再从这个库拉取 .o，
# 于是 UARTCallback.c.obj 整份被丢弃，固件里跑的是 HAL 的空回调：
#   - TC 回调空 → RS485_DIR 永远拉不低 → 收发器一直占着总线，从站应答收不到
#   - RX 回调空 → 收到的字节永远进不了环形缓冲 → 每帧都超时
# 直接作为可执行文件的源文件参与链接，强符号才会覆盖弱符号。
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/UARTCallback.c)

target_include_directories(modbus PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../../common
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(modbus PUBLIC stm32cubemx)

target_include_directories(modbus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../integration
    ${CMAKE_CURRENT_SOURCE_DIR}/../upgrade
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../shared
)
