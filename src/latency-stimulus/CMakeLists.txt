if(EXISTS "/run/.containerenv" OR EXISTS "/.dockerenv")
    message(FATAL_ERROR
        "latency-stimulus must be configured on the CachyOS host, not in Distrobox.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LATENCY_SDL REQUIRED IMPORTED_TARGET sdl3)

add_executable(latency-stimulus src/main.cpp)
target_link_libraries(latency-stimulus PRIVATE PkgConfig::LATENCY_SDL)
target_compile_features(latency-stimulus PRIVATE cxx_std_20)
target_compile_options(latency-stimulus PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
