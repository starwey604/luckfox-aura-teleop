add_executable(aura-sender src/main.c)

target_link_libraries(aura-sender PRIVATE teleop-common)
target_compile_features(aura-sender PRIVATE c_std_11)
target_compile_options(aura-sender PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS aura-sender RUNTIME DESTINATION bin)
