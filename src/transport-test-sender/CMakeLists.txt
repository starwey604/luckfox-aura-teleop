# The synthetic source is deliberately separate from aura-sender: it keeps the
# host UDP transport regression test independent of the target-only RK MPI SDK.
add_executable(transport-test-sender ../aura-sender/src/main.cpp)

target_link_libraries(transport-test-sender PRIVATE teleop-common)
target_compile_features(transport-test-sender PRIVATE cxx_std_20)
target_compile_options(transport-test-sender PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
