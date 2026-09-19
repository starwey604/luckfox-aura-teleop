add_executable(host-viewer src/main.c)
target_link_libraries(host-viewer PRIVATE teleop-common)
target_compile_features(host-viewer PRIVATE c_std_11)
