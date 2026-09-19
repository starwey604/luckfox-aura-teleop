if(NOT DEFINED AURA_SDK_ROOT OR NOT EXISTS "${AURA_SDK_ROOT}/media/out/include/rk_mpi_venc.h")
    message(FATAL_ERROR
        "aura-sender requires the Aura SDK media headers. Configure with "
        "cmake/aura-toolchain.cmake and set -DAURA_SDK_ROOT=/path/to/aura-sdk.")
endif()

set(AURA_MEDIA_INCLUDE_DIR "${AURA_SDK_ROOT}/media/out/include")
set(AURA_MEDIA_LIBRARY_DIR "${AURA_SDK_ROOT}/media/out/lib")
set(AURA_TARGET_LIBRARY_DIR "${AURA_SDK_ROOT}/media/out/root/usr/lib")
set(AURA_ROCKIT_LIBRARY "${AURA_MEDIA_LIBRARY_DIR}/librockit.so")
set(AURA_MPP_LIBRARY "${AURA_MEDIA_LIBRARY_DIR}/librockchip_mpp.so.0")
set(AURA_ALSA_LIBRARY "${AURA_TARGET_LIBRARY_DIR}/libasound.so")

foreach(required_file IN ITEMS "${AURA_ROCKIT_LIBRARY}" "${AURA_MPP_LIBRARY}" "${AURA_ALSA_LIBRARY}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Aura SDK media library is missing: ${required_file}")
    endif()
endforeach()

add_executable(aura-sender src/mpp_sender.cpp)

target_include_directories(aura-sender SYSTEM PRIVATE "${AURA_MEDIA_INCLUDE_DIR}")
target_link_libraries(aura-sender PRIVATE
    teleop-common
    "${AURA_ROCKIT_LIBRARY}"
    "${AURA_MPP_LIBRARY}"
    "${AURA_ALSA_LIBRARY}"
    pthread
    dl)
target_link_options(aura-sender PRIVATE
    "-Wl,-rpath-link,${AURA_MEDIA_LIBRARY_DIR}"
    "-Wl,-rpath-link,${AURA_TARGET_LIBRARY_DIR}")
target_compile_features(aura-sender PRIVATE cxx_std_20)
target_compile_options(aura-sender PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

install(TARGETS aura-sender RUNTIME DESTINATION bin)
