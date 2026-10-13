#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace teleop {

class DecodedFrameSink {
public:
    virtual ~DecodedFrameSink() = default;
    virtual void present(const AVFrame& frame) = 0;
    virtual std::string_view timing_stage() const { return "present_call"; }
};

struct DecodeStatistics {
    std::uint64_t decoded_frames{};
    std::uint64_t hardware_frames{};
};

class H264VaapiDecoder {
public:
    explicit H264VaapiDecoder(std::string_view vaapi_device, DecodedFrameSink* sink = nullptr,
                              bool use_vaapi = true, std::uint16_t metrics_stream_id = 0U);
    ~H264VaapiDecoder();

    H264VaapiDecoder(const H264VaapiDecoder&) = delete;
    H264VaapiDecoder& operator=(const H264VaapiDecoder&) = delete;

    void submit_access_unit(std::span<const std::byte> access_unit, std::uint64_t ready_ns = 0U);
    void flush();

    [[nodiscard]] const DecodeStatistics& statistics() const;
    [[nodiscard]] AVCodecContext* codec_context() const;

private:
    void drain();

    AVCodecContext* context_{};
    AVBufferRef* hardware_device_{};
    DecodedFrameSink* sink_{};
    bool use_vaapi_{true};
    DecodeStatistics statistics_{};
    std::string metrics_prefix_;
    std::map<std::int64_t, std::uint64_t> submit_times_;
};

// Rockit low-delay VENC exposes a bytestream fragment for each slice callback,
// while the hardware decoder requires complete H.264 access units. This parser
// preserves that distinction at the receiver boundary.
class H264AnnexBParser {
public:
    explicit H264AnnexBParser(H264VaapiDecoder& decoder);
    ~H264AnnexBParser();

    H264AnnexBParser(const H264AnnexBParser&) = delete;
    H264AnnexBParser& operator=(const H264AnnexBParser&) = delete;

    void submit_chunk(std::span<const std::byte> chunk, std::uint64_t ready_ns = 0U);
    void flush();

private:
    void parse_input(const std::uint8_t* input, int input_size, std::uint64_t ready_ns);

    H264VaapiDecoder& decoder_;
    AVCodecParserContext* parser_{};
    std::vector<std::byte> padded_input_;
};

} // namespace teleop
