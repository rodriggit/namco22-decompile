# devkitPro Wii toolchain skeleton.
#
# Typical usage:
#   cmake -S timecris -B build-wii \
#     -DTC_WII_PORT=ON \
#     -DDEVKITPRO=/path/to/devkitPro \
#     -DCMAKE_TOOLCHAIN_FILE=timecris/wii/toolchain-devkitpro.cmake
#
# This toolchain intentionally keeps the native Wii target isolated from the
# desktop build so the host SDL/OpenGL path remains unchanged.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc)
set(CMAKE_CROSSCOMPILING TRUE)

if(NOT DEFINED DEVKITPRO AND DEFINED ENV{DEVKITPRO})
    set(DEVKITPRO "$ENV{DEVKITPRO}")
elseif(NOT DEFINED DEVKITPRO)
    message(FATAL_ERROR "DEVKITPRO must be set before using this toolchain file.")
endif()

set(CMAKE_FIND_ROOT_PATH
    "${DEVKITPRO}"
    "${DEVKITPRO}/portlibs/ppc"
    "${DEVKITPRO}/portlibs/wii")
list(APPEND CMAKE_PREFIX_PATH
    "${DEVKITPRO}/portlibs/ppc"
    "${DEVKITPRO}/portlibs/wii")    
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "${DEVKITPRO}/devkitPPC/bin/powerpc-eabi-gcc")
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "${DEVKITPRO}/devkitPPC/bin/powerpc-eabi-g++")
endif()
set(CMAKE_AR "${DEVKITPRO}/devkitPPC/bin/powerpc-eabi-ar")
set(CMAKE_RANLIB "${DEVKITPRO}/devkitPPC/bin/powerpc-eabi-ranlib")

set(CMAKE_C_FLAGS_INIT "-mrvl -mcpu=750 -meabi -mhard-float")
set(CMAKE_CXX_FLAGS_INIT "-mrvl -mcpu=750 -meabi -mhard-float")

# Keep this separate from the desktop CMake tree to avoid mixing Wii and host
# build assumptions in the same project configuration.
