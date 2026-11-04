cmake_minimum_required(VERSION 3.22)

project(modbus-mqtt-gateway LANGUAGES C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(GATEWAY_FIRMWARE "app" CACHE STRING " Firmware to build: app or bootloader")

if(GATEWAY_FIRMWARE STREQUAL "app")
    add_subdirectory(firmware/app)
elseif(GATEWAY_FIRMWARE STREQUAL "bootloader")
    add_subdirectory(firmware/bootloader)
else()
    message(FATAL_ERROR "GATEWAY_FIRMWARE must be either 'app' or 'bootloader'")
endif()