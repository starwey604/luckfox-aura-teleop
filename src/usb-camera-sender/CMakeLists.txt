if(EXISTS "/run/.containerenv" OR EXISTS "/.dockerenv")
    message(FATAL_ERROR
        "usb-camera-sender must be configured on the CachyOS host, not in Distrobox.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(USB_CAMERA_FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)

add_executable(usb-camera-sender src/main.cpp)
target_link_libraries(usb-camera-sender PRIVATE teleop-common PkgConfig::USB_CAMERA_FFMPEG)
target_compile_features(usb-camera-sender PRIVATE cxx_std_20)
target_compile_options(usb-camera-sender PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
