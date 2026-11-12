cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME bootloader)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Boot固定64KiB；Debug也保留体积优化及调试信息，避免-O0使合法配置无法链接。
# 放在生成驱动子目录之前，使HAL与Boot采用同一优化口径；AppDebug不受影响。
add_compile_options($<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C>>:-Os>)
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:-Os>)

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_jump.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_app_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_package.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/upgrade_package.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_package_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_upgrade_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_package_receive.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_recovery_receive.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/upgrade_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/upgrade_transaction.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/upgrade_trust.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_install.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_trial.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_watchdog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/watchdog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_recovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_recovery_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/upgrade_mailbox.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/maint_frame.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/bsp/storage"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# 安全包认证所需的 Mbed TLS 源码，算法由 boot_mbedtls_config.h 裁剪。
set(BOOT_MBEDTLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls")

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    "${BOOT_MBEDTLS_DIR}/library/sha256.c"
    "${BOOT_MBEDTLS_DIR}/library/aes.c"
    "${BOOT_MBEDTLS_DIR}/library/gcm.c"
    "${BOOT_MBEDTLS_DIR}/library/block_cipher.c"
    "${BOOT_MBEDTLS_DIR}/library/ecdsa.c"
    "${BOOT_MBEDTLS_DIR}/library/ecp.c"
    "${BOOT_MBEDTLS_DIR}/library/ecp_curves.c"
    "${BOOT_MBEDTLS_DIR}/library/bignum.c"
    "${BOOT_MBEDTLS_DIR}/library/bignum_core.c"
    "${BOOT_MBEDTLS_DIR}/library/bignum_mod.c"
    "${BOOT_MBEDTLS_DIR}/library/bignum_mod_raw.c"
    "${BOOT_MBEDTLS_DIR}/library/constant_time.c"
    "${BOOT_MBEDTLS_DIR}/library/asn1parse.c"
    "${BOOT_MBEDTLS_DIR}/library/asn1write.c"
    "${BOOT_MBEDTLS_DIR}/library/platform_util.c"
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${BOOT_MBEDTLS_DIR}/include"
)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    MBEDTLS_CONFIG_FILE=\"boot_mbedtls_config.h\"
)

# 串口收包自测显式启用测试密钥；测试密钥不进入 Git 或正式构建。
option(BOOT_PACKAGE_RECEIVE_TEST "Enable local serial package receive test" OFF)
option(BOOT_FLASH_READ_DIAGNOSTIC "Run read-only diagnostic instead of receiving packages" OFF)
if(BOOT_FLASH_READ_DIAGNOSTIC AND NOT BOOT_PACKAGE_RECEIVE_TEST)
    message(FATAL_ERROR "Read diagnostic requires the local BootReceiveTest build")
endif()
if(BOOT_PACKAGE_RECEIVE_TEST)
    set(BOOT_TEST_KEYS_DIR "${CMAKE_SOURCE_DIR}/.local/upgrade-fixture")
    if(NOT EXISTS "${BOOT_TEST_KEYS_DIR}/boot_test_keys.h")
        message(FATAL_ERROR "Run .local/tools/export_test_keys.py before BootReceiveTest")
    endif()
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${BOOT_TEST_KEYS_DIR}")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE BOOT_PACKAGE_RECEIVE_TEST=1)
    if(BOOT_FLASH_READ_DIAGNOSTIC)
        target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE BOOT_FLASH_READ_DIAGNOSTIC=1)
    endif()
endif()
