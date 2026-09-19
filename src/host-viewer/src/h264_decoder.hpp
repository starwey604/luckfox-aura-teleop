#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace teleop {

struct DecodeStatistics {
    std::uint64_t decoded_frames{};
    std::uint64_t hardware_frames{};
};

class H264VaapiDecoder {
public:
    explicit H264VaapiDecoder(std::string_view vaapi_device);
    ~H264VaapiDecoder();

    H264VaapiDecoder(const H264VaapiDecoder&) = delete;
    H264VaapiDecoder& operator=(const H264VaapiDecoder&) = delete;

    void submit_access_unit(std::span<const std::byte> access_unit);
    void flush();

    [[nodiscard]] const DecodeStatistics& statistics() const;
    [[nodiscard]] AVCodecContext* codec_context() const;

private:
    void drain();

    AVCodecContext* context_{};
    AVBufferRef* hardware_device_{};
    DecodeStatistics statistics_{};
};

} // namespace teleop
