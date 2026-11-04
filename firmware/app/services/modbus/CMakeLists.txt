set(MODBUS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/Modbus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UARTCallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_app.c
)

add_library(modbus STATIC ${MODBUS_SRCS})

target_include_directories(modbus PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Config
)

target_link_libraries(modbus PUBLIC stm32cubemx)