add_library(teleop-common STATIC
    packetizer.cpp
    performance.cpp
    protocol.cpp
    reassembler.cpp
    statistics.cpp
    udp_socket.cpp
)

target_include_directories(teleop-common PUBLIC include)
target_compile_features(teleop-common PUBLIC cxx_std_20)
target_compile_options(teleop-common PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
