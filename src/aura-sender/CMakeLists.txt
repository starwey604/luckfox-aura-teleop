add_executable(aura-sender src/main.cpp)

target_link_libraries(aura-sender PRIVATE teleop-common)
target_compile_features(aura-sender PRIVATE cxx_std_20)
target_compile_options(aura-sender PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

install(TARGETS aura-sender RUNTIME DESTINATION bin)
