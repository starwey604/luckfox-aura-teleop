cmake_minimum_required(VERSION 3.20)

project(luckfox_aura_teleop VERSION 0.1.0 LANGUAGES C CXX)

option(TELEOP_BUILD_AURA_SENDER "Build the Aura sender executable" ON)
option(TELEOP_BUILD_TRANSPORT_TEST_SENDER "Build the host synthetic transport sender" OFF)
option(TELEOP_BUILD_HOST_VIEWER "Build the host viewer executable" OFF)
option(TELEOP_BUILD_USB_CAMERA_SENDER "Build the CachyOS USB-camera sender" OFF)
option(TELEOP_BUILD_LATENCY_STIMULUS "Build the CachyOS optical latency test pattern" OFF)
option(TELEOP_BUILD_TESTS "Build transport protocol tests" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src/common)

if(TELEOP_BUILD_AURA_SENDER)
    add_subdirectory(src/aura-sender)
endif()

if(TELEOP_BUILD_TRANSPORT_TEST_SENDER)
    add_subdirectory(src/transport-test-sender)
endif()

if(TELEOP_BUILD_HOST_VIEWER)
    add_subdirectory(src/host-viewer)
endif()

if(TELEOP_BUILD_USB_CAMERA_SENDER)
    add_subdirectory(src/usb-camera-sender)
endif()

if(TELEOP_BUILD_LATENCY_STIMULUS)
    add_subdirectory(src/latency-stimulus)
endif()

if(TELEOP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/protocol)
endif()
