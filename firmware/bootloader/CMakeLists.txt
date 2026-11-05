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
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/boot_package_flash.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
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

# Temporary Mbed TLS size experiment
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
