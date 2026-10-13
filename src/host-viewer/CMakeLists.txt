if(EXISTS "/run/.containerenv" OR EXISTS "/.dockerenv")
    message(FATAL_ERROR
        "host-viewer must be configured on the CachyOS host, not in Distrobox. "
        "Use tools/build-host from the host shell.")
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
pkg_check_modules(PRESENTER REQUIRED IMPORTED_TARGET sdl3 vulkan libplacebo)

add_executable(host-viewer
    src/h264_decoder.cpp
    src/libplacebo_ffmpeg_bridge.c
    src/main.cpp
    src/receive_observer.cpp
    src/thread_policy.cpp
    src/viewer_pipeline.cpp
    src/vulkan_presenter.cpp
)
target_link_libraries(host-viewer PRIVATE
    teleop-common
    Threads::Threads
    PkgConfig::FFMPEG
    PkgConfig::PRESENTER
)
target_compile_features(host-viewer PRIVATE cxx_std_20)
target_compile_options(host-viewer PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

if(TELEOP_BUILD_TESTS)
    enable_testing()
    add_executable(receive-observer-test
        ../../tests/latency/receive_observer_test.cpp
        src/receive_observer.cpp
    )
    target_include_directories(receive-observer-test PRIVATE src)
    target_link_libraries(receive-observer-test PRIVATE teleop-common)
    target_compile_features(receive-observer-test PRIVATE cxx_std_20)
    target_compile_options(receive-observer-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME receive-observer-test COMMAND receive-observer-test)
    set_tests_properties(receive-observer-test PROPERTIES ENVIRONMENT "TELEOP_METRICS=1" TIMEOUT 5)
endif()
