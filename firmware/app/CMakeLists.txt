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
set(CMAKE_PROJECT_NAME modbus-mqtt-gateway)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)
if(GATEWAY_ENABLE_M2)
    add_subdirectory(Middlewares/LwIP)
endif()
add_subdirectory(bsp)
add_subdirectory(integration)
if(GATEWAY_ENABLE_M2)
    add_subdirectory(services/mqtt)
endif()
add_subdirectory(services/modbus)
add_subdirectory(services/storage)
add_subdirectory(services/config)
if(GATEWAY_ENABLE_M2)
    add_subdirectory(services/network_upgrade)
endif()

# 云端配置接入后需适配448KiB App区；仅优化配置解析和MQTT代码体积，保留调试信息。
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/services/config/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/services/config/mini_json.c
    ${CMAKE_CURRENT_SOURCE_DIR}/services/mqtt/mqtt_client.c
    PROPERTIES COMPILE_OPTIONS "-Os"
)

# Configure the application vector table offset.
set(GATEWAY_APP_VECTOR_OFFSET "0x00000000" CACHE STRING "Application vector table offset")

target_compile_definitions(STM32_Drivers PRIVATE USER_VECT_TAB_ADDRESS VECT_TAB_OFFSET=${GATEWAY_APP_VECTOR_OFFSET})

# 保留生成的_sbrk边界检查，由现有内存模块记录C堆实际扩展峰值。
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "LINKER:--wrap=_sbrk")


# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_CURRENT_LIST_DIR}/services/upgrade/upgrade_stage.c
    ${CMAKE_CURRENT_LIST_DIR}/services/upgrade/upgrade_stage_auth.c
    ${CMAKE_CURRENT_LIST_DIR}/services/upgrade/upgrade_quiesce.c
    ${CMAKE_CURRENT_LIST_DIR}/services/upgrade/upgrade_storage.c
    ${CMAKE_CURRENT_LIST_DIR}/services/upgrade/upgrade_entry.c
    ${CMAKE_CURRENT_LIST_DIR}/services/upgrade/upgrade_tf.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared/upgrade_package.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared/upgrade_state.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared/upgrade_transaction.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared/upgrade_trust.c
)

# App与Boot使用同一份密码库，分别按各自配置裁剪。
set(UPGRADE_MBEDTLS_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/mbedtls)

# 基础安全包源（升级验签 + TLS 共用）
set(MBEDTLS_BASE_SOURCES
    ${UPGRADE_MBEDTLS_DIR}/library/sha256.c
    ${UPGRADE_MBEDTLS_DIR}/library/aes.c
    ${UPGRADE_MBEDTLS_DIR}/library/gcm.c
    ${UPGRADE_MBEDTLS_DIR}/library/block_cipher.c
    ${UPGRADE_MBEDTLS_DIR}/library/ecdsa.c
    ${UPGRADE_MBEDTLS_DIR}/library/ecp.c
    ${UPGRADE_MBEDTLS_DIR}/library/ecp_curves.c
    ${UPGRADE_MBEDTLS_DIR}/library/bignum.c
    ${UPGRADE_MBEDTLS_DIR}/library/bignum_core.c
    ${UPGRADE_MBEDTLS_DIR}/library/bignum_mod.c
    ${UPGRADE_MBEDTLS_DIR}/library/bignum_mod_raw.c
    ${UPGRADE_MBEDTLS_DIR}/library/constant_time.c
    ${UPGRADE_MBEDTLS_DIR}/library/asn1parse.c
    ${UPGRADE_MBEDTLS_DIR}/library/asn1write.c
    ${UPGRADE_MBEDTLS_DIR}/library/platform_util.c
)

# 升级验签源文件列表（用 upgrade_stage_mbedtls_config.h 或 TLS 配置编译）
set(UPGRADE_SOURCE_FILES
    ${CMAKE_CURRENT_LIST_DIR}/services/upgrade/upgrade_tf.c
    ${CMAKE_CURRENT_LIST_DIR}/services/upgrade/upgrade_stage.c
    ${CMAKE_CURRENT_LIST_DIR}/services/upgrade/upgrade_stage_auth.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared/upgrade_package.c
)

if(GATEWAY_ENABLE_M2)
    # TLS固定输入区使用同版本库计算的完整缓冲长度，避免手工估计记录开销。
    set_property(SOURCE ${CMAKE_CURRENT_LIST_DIR}/services/network_upgrade/https_tls.c
        APPEND PROPERTY INCLUDE_DIRECTORIES ${UPGRADE_MBEDTLS_DIR}/library)
    # ===== M2 启用：编译 TLS 客户端 =====
    # 统一使用 https_tls_mbedtls_config.h（包含 upgrade_mbedtls_config.h + BLOCK_CIPHER_C + TLS 宏）
    # 使 platform_util.c 的 mbedtls_platform_gmtime_r 等函数被编译（需 HAVE_TIME_DATE）
    set(TLS_MBEDTLS_SOURCES
        ${UPGRADE_MBEDTLS_DIR}/library/ssl_tls.c
        ${UPGRADE_MBEDTLS_DIR}/library/ssl_tls12_client.c
        ${UPGRADE_MBEDTLS_DIR}/library/ssl_client.c
        ${UPGRADE_MBEDTLS_DIR}/library/ssl_msg.c
        ${UPGRADE_MBEDTLS_DIR}/library/ssl_ciphersuites.c
        ${UPGRADE_MBEDTLS_DIR}/library/x509.c
        ${UPGRADE_MBEDTLS_DIR}/library/x509_crt.c
        ${UPGRADE_MBEDTLS_DIR}/library/x509_crl.c
        ${UPGRADE_MBEDTLS_DIR}/library/pk.c
        ${UPGRADE_MBEDTLS_DIR}/library/pk_wrap.c
        ${UPGRADE_MBEDTLS_DIR}/library/pkparse.c
        ${UPGRADE_MBEDTLS_DIR}/library/pk_ecc.c
        ${UPGRADE_MBEDTLS_DIR}/library/rsa.c
        ${UPGRADE_MBEDTLS_DIR}/library/rsa_alt_helpers.c
        ${UPGRADE_MBEDTLS_DIR}/library/cipher.c
        ${UPGRADE_MBEDTLS_DIR}/library/cipher_wrap.c
        ${UPGRADE_MBEDTLS_DIR}/library/entropy.c
        ${UPGRADE_MBEDTLS_DIR}/library/ctr_drbg.c
        ${UPGRADE_MBEDTLS_DIR}/library/ecdh.c
        ${UPGRADE_MBEDTLS_DIR}/library/pem.c
        ${UPGRADE_MBEDTLS_DIR}/library/base64.c
        ${UPGRADE_MBEDTLS_DIR}/library/md.c
        ${UPGRADE_MBEDTLS_DIR}/library/oid.c
        ${UPGRADE_MBEDTLS_DIR}/library/platform.c
    )
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${MBEDTLS_BASE_SOURCES} ${TLS_MBEDTLS_SOURCES})
    # 统一 TLS 配置（基础 + TLS + 升级验签 + 下载器 + 桩）
    set_source_files_properties(
        ${MBEDTLS_BASE_SOURCES} ${TLS_MBEDTLS_SOURCES}
        ${UPGRADE_SOURCE_FILES}
        ${CMAKE_CURRENT_LIST_DIR}/services/network_upgrade/https_tls.c
        ${CMAKE_CURRENT_LIST_DIR}/services/network_upgrade/https_tls_stubs.c
        ${CMAKE_CURRENT_LIST_DIR}/services/network_upgrade/network_upgrade_downloader.c
        PROPERTIES COMPILE_DEFINITIONS MBEDTLS_CONFIG_FILE="https_tls_mbedtls_config.h"
    )
else()
    # ===== M2 禁用：仅升级验签，无 TLS =====
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${MBEDTLS_BASE_SOURCES})
    set_source_files_properties(
        ${MBEDTLS_BASE_SOURCES}
        ${UPGRADE_SOURCE_FILES}
        PROPERTIES COMPILE_DEFINITIONS MBEDTLS_CONFIG_FILE="upgrade_stage_mbedtls_config.h"
    )
endif()

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_LIST_DIR}/common
    ${CMAKE_CURRENT_LIST_DIR}/services/upgrade
    ${CMAKE_CURRENT_LIST_DIR}/../shared                # upgrade_mbedtls_config.h + https_tls_mbedtls_config.h
    ${UPGRADE_MBEDTLS_DIR}/include
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # M2 开关，供 app_entry.c 等源码裁剪以太网初始化
    GATEWAY_ENABLE_M2=$<BOOL:${GATEWAY_ENABLE_M2}>
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
    modbus
)

if(GATEWAY_ENABLE_M2)
    target_link_libraries(${CMAKE_PROJECT_NAME} lwip m2_ethernet)
endif()


# ========== POST_BUILD：编译elf成功之后自动执行的命令 ==========
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.hex   # elf转hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin # elf转bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>  # 打印Flash、RAM占用大小
)
