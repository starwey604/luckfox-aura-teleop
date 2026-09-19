add_library(teleop-common STATIC
    packetizer.c
    statistics.c
)

target_include_directories(teleop-common PUBLIC include)
target_compile_features(teleop-common PUBLIC c_std_11)
target_compile_options(teleop-common PRIVATE -Wall -Wextra -Wpedantic)
