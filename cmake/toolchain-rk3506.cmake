# RK3506 / ARMv7 cross toolchain for the native Linux branch.
#
# Override the toolchain root with the TOOLCHAIN_DIR environment variable or
# -DTOOLCHAIN_DIR=/path/to/gcc-arm-... if it is installed elsewhere.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED TOOLCHAIN_DIR OR TOOLCHAIN_DIR STREQUAL "")
    if(DEFINED ENV{TOOLCHAIN_DIR})
        set(TOOLCHAIN_DIR "$ENV{TOOLCHAIN_DIR}")
    else()
        set(TOOLCHAIN_DIR "$ENV{HOME}/rk3506_sdk/prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf")
    endif()
endif()

set(CMAKE_C_COMPILER   "${TOOLCHAIN_DIR}/bin/arm-none-linux-gnueabihf-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_DIR}/bin/arm-none-linux-gnueabihf-g++")

set(CMAKE_FIND_ROOT_PATH "${TOOLCHAIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
