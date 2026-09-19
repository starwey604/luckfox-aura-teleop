cmake_minimum_required(VERSION 3.20)

project(luckfox_aura_teleop VERSION 0.1.0 LANGUAGES C)

option(TELEOP_BUILD_AURA_SENDER "Build the Aura sender executable" ON)
option(TELEOP_BUILD_HOST_VIEWER "Build the host viewer executable" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(src/common)

if(TELEOP_BUILD_AURA_SENDER)
    add_subdirectory(src/aura-sender)
endif()

if(TELEOP_BUILD_HOST_VIEWER)
    add_subdirectory(src/host-viewer)
endif()
