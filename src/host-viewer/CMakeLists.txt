if(EXISTS "/run/.containerenv" OR EXISTS "/.dockerenv")
    message(FATAL_ERROR
        "host-viewer must be configured on the CachyOS host, not in Distrobox. "
        "Use tools/build-host from the host shell.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)

add_executable(host-viewer
    src/h264_decoder.cpp
    src/main.cpp
)
target_link_libraries(host-viewer PRIVATE teleop-common PkgConfig::FFMPEG)
target_compile_features(host-viewer PRIVATE cxx_std_20)
target_compile_options(host-viewer PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
