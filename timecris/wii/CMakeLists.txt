cmake_minimum_required(VERSION 3.16)

# Wii-only Time Crisis backend target.
#
# This is intentionally limited to the native backend layer. The game logic stays
# in the shared engine and the Time Crisis sources; only the host/runtime layer is
# swapped for a devkitPro-native backend.
#
# Expected usage:
#   cmake -S timecris/wii -B build-wii -DDEVKITPRO=/path/to/devkitPro \
#         -DCMAKE_TOOLCHAIN_FILE=timecris/wii/toolchain-devkitpro.cmake
#
# This file is a stub and is meant to be expanded once the real Wii toolchain
# and native GX/AX code path are wired into the project.

project(timecris_wii_backend C)

option(TC_WII_PORT "Build the native Time Crisis Wii backend" OFF)
if(NOT TC_WII_PORT)
    message(FATAL_ERROR "Set -DTC_WII_PORT=ON to enable the native Wii backend target.")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "This CMakeLists is for the Wii-native devkitPro backend only; do not build it from the desktop tree.")
endif()

if(NOT DEFINED DEVKITPRO)
    message(FATAL_ERROR "DEVKITPRO must be set to the devkitPro installation root.")
endif()

if(NOT EXISTS "${DEVKITPRO}/libogc")
    message(FATAL_ERROR "devkitPro libogc not found under ${DEVKITPRO}.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_SYSTEM_NAME Wii)
message(STATUS "Configuring native Wii backend for Time Crisis")
add_compile_definitions(__WII__)

add_library(timecris_wii_port STATIC
    port.c
)

target_include_directories(timecris_wii_port
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(timecris_wii_port PRIVATE
    -mrvl -mcpu=750 -meabi -mhard-float
)

target_link_libraries(timecris_wii_port
    PRIVATE
        ogc
        wiiuse
        gcc
)

# The final native build should compile only the Time Crisis subset and keep all
# desktop Linux/Windows targets untouched.
