cmake_minimum_required(VERSION 3.22)

project(modbus-mqtt-gateway LANGUAGES C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(GATEWAY_FIRMWARE "app" CACHE STRING " Firmware to build: app or bootloader")

# 是否编入 M2（以太网/LwIP/MQTT）代码。关掉后只保留 M1/M3，便于单独测 M3：
#   不编译 mqtt_client.c，不链接 lwip / m2_ethernet，也不调用 m2_ethernet_init。
option(GATEWAY_ENABLE_M2 "Build M2 (Ethernet/LwIP/MQTT) sources" ON)

if(GATEWAY_FIRMWARE STREQUAL "app")
    add_subdirectory(firmware/app)
elseif(GATEWAY_FIRMWARE STREQUAL "bootloader")
    add_subdirectory(firmware/bootloader)
else()
    message(FATAL_ERROR "GATEWAY_FIRMWARE must be either 'app' or 'bootloader'")
endif()