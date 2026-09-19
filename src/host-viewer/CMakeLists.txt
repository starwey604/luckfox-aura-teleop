add_executable(host-viewer src/main.cpp)
target_link_libraries(host-viewer PRIVATE teleop-common)
target_compile_features(host-viewer PRIVATE cxx_std_20)
target_compile_options(host-viewer PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
