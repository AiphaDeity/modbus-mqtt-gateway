cmake_minimum_required(VERSION 3.22)

project(modbus-mqtt-gateway LANGUAGES C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(GATEWAY_FIRMWARE "app" CACHE STRING " Firmware to build: app or bootloader")

# 升级状态记录的格式版本；Boot、App 和制包工具须使用相同版本。
set(GATEWAY_UPGRADE_PERSISTENCE_VERSION "2" CACHE STRING "Upgrade persistence version (2 only)")
set_property(CACHE GATEWAY_UPGRADE_PERSISTENCE_VERSION PROPERTY STRINGS 2)
if(NOT GATEWAY_UPGRADE_PERSISTENCE_VERSION STREQUAL "2")
    message(FATAL_ERROR "Only upgrade persistence version 2 is supported; reconfigure with version 2")
endif()

add_compile_definitions(GATEWAY_UPGRADE_PERSISTENCE_VERSION=${GATEWAY_UPGRADE_PERSISTENCE_VERSION})

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

# 普通构建只读Boot预留信任记录；可指定其他保护区实现，源码不嵌入密钥。
set(GATEWAY_UPGRADE_TRUST_SOURCE "" CACHE FILEPATH "Protected-storage upgrade trust provider")
set(upgrade_trust_source "${GATEWAY_UPGRADE_TRUST_SOURCE}")
if(NOT upgrade_trust_source AND NOT BOOT_PACKAGE_RECEIVE_TEST)
    set(upgrade_trust_source "${CMAKE_CURRENT_SOURCE_DIR}/firmware/shared/upgrade_trust_flash.c")
endif()
if(upgrade_trust_source)
    if(BOOT_PACKAGE_RECEIVE_TEST)
        message(FATAL_ERROR "Production trust provider cannot be combined with the test trust root")
    endif()
    if(NOT EXISTS "${upgrade_trust_source}")
        message(FATAL_ERROR "Upgrade trust provider source does not exist")
    endif()
    if(GATEWAY_FIRMWARE STREQUAL "bootloader")
        target_sources(bootloader PRIVATE "${upgrade_trust_source}")
    else()
        target_sources(modbus-mqtt-gateway PRIVATE "${upgrade_trust_source}")
        set_source_files_properties("${upgrade_trust_source}"
            TARGET_DIRECTORY modbus-mqtt-gateway PROPERTIES
            COMPILE_DEFINITIONS MBEDTLS_CONFIG_FILE=\"upgrade_mbedtls_config.h\")
    endif()
endif()
