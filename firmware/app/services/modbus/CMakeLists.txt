set(MODBUS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/Modbus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UARTCallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mb_poll.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mb_slave.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mb_api.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mb_pointmap.c
)

add_library(modbus STATIC ${MODBUS_SRCS})

target_include_directories(modbus PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Config
)

target_link_libraries(modbus PUBLIC stm32cubemx)
