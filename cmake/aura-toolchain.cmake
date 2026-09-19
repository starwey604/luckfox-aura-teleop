# Cross-compilation toolchain for the Aura RV1126B AArch64 SDK.
#
# Usage:
#   cmake -S . -B build/aura -DCMAKE_TOOLCHAIN_FILE=cmake/aura-toolchain.cmake
#   cmake -S . -B build/aura -DCMAKE_TOOLCHAIN_FILE=cmake/aura-toolchain.cmake \
#       -DAURA_SDK_ROOT=/path/to/aura-sdk
#   cmake -S . -B build/aura -DCMAKE_TOOLCHAIN_FILE=cmake/aura-toolchain.cmake \
#       -DAURA_TOOLCHAIN_ROOT=/path/to/aarch64-rockchip1240-linux-gnu

get_filename_component(TELEOP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(AURA_SDK_ROOT "${TELEOP_ROOT}/../aura-sdk" CACHE PATH
    "Path to the Aura SDK checkout")
set(AURA_TOOLCHAIN_ROOT
    "${AURA_SDK_ROOT}/tools/linux/toolchain/aarch64-rockchip1240-linux-gnu"
    CACHE PATH "Path to the Rockchip AArch64 toolchain")

set(AURA_TOOLCHAIN_BIN "${AURA_TOOLCHAIN_ROOT}/bin")
set(AURA_TOOLCHAIN_TRIPLE "aarch64-rockchip1240-linux-gnu")
set(AURA_TOOLCHAIN_SYSROOT
    "${AURA_TOOLCHAIN_ROOT}/${AURA_TOOLCHAIN_TRIPLE}/sysroot")

foreach(required_path IN ITEMS
        "${AURA_TOOLCHAIN_BIN}/${AURA_TOOLCHAIN_TRIPLE}-gcc"
        "${AURA_TOOLCHAIN_BIN}/${AURA_TOOLCHAIN_TRIPLE}-g++"
        "${AURA_TOOLCHAIN_SYSROOT}")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR
            "Aura toolchain is incomplete: ${required_path}\n"
            "Set -DAURA_SDK_ROOT=... or -DAURA_TOOLCHAIN_ROOT=...")
    endif()
endforeach()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT "${AURA_TOOLCHAIN_SYSROOT}")
set(CMAKE_C_COMPILER "${AURA_TOOLCHAIN_BIN}/${AURA_TOOLCHAIN_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${AURA_TOOLCHAIN_BIN}/${AURA_TOOLCHAIN_TRIPLE}-g++")
set(CMAKE_AR "${AURA_TOOLCHAIN_BIN}/${AURA_TOOLCHAIN_TRIPLE}-ar")
set(CMAKE_RANLIB "${AURA_TOOLCHAIN_BIN}/${AURA_TOOLCHAIN_TRIPLE}-ranlib")
set(CMAKE_STRIP "${AURA_TOOLCHAIN_BIN}/${AURA_TOOLCHAIN_TRIPLE}-strip")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
