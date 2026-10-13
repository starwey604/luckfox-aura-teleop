// Native VAAPI experiment using the production decoder; never changes viewer defaults.
#include "h264_decoder.hpp"
#include <teleop/performance.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    try {
        if (argc != 5) throw std::runtime_error("usage: probe MODE H264 OFFSETS PERIOD_US");
        const std::string mode{argv[1]};
        if (mode != "stream" && mode != "complete-parser" && mode != "direct")
            throw std::runtime_error("invalid mode");
        const auto period = std::chrono::microseconds{std::stoll(argv[4])};
        if (period.count() < 0) throw std::runtime_error("negative period");
        std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
        if (!input || input.tellg() <= 0) throw std::runtime_error("empty/missing H.264 input");
        std::vector<std::byte> data(static_cast<std::size_t>(input.tellg()));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!input) throw std::runtime_error("could not read H.264 input");
        std::ifstream offsets(argv[3]);
        if (!offsets) throw std::runtime_error("missing AU offsets");

        teleop::H264VaapiDecoder decoder{"/dev/dri/renderD128"};
        std::unique_ptr<teleop::H264AnnexBParser> stream_parser;
        std::unique_ptr<AVCodecParserContext, decltype(&av_parser_close)>
            complete_parser{nullptr, &av_parser_close};
        if (mode == "stream") stream_parser = std::make_unique<teleop::H264AnnexBParser>(decoder);
        if (mode == "complete-parser") {
            complete_parser.reset(av_parser_init(AV_CODEC_ID_H264));
            if (!complete_parser) throw std::runtime_error("could not open complete-AU parser");
            complete_parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
        }
        std::vector<std::uint64_t> after_inputs;
        std::vector<std::byte> padded;
        const auto start = std::chrono::steady_clock::now();
        std::size_t position{}, size{};
        while (offsets >> position >> size) {
            if (size == 0 || position > data.size() || size > data.size() - position ||
                size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                throw std::runtime_error("invalid AU offsets");
            std::this_thread::sleep_until(start + period * after_inputs.size());
            const auto ready_ns = teleop::performance_time_ns();
            const auto au = std::span<const std::byte>{data}.subspan(position, size);
            if (stream_parser) {
                stream_parser->submit_chunk(au, ready_ns);
            } else if (complete_parser) {
                padded.assign(size + AV_INPUT_BUFFER_PADDING_SIZE, std::byte{0});
                std::copy(au.begin(), au.end(), padded.begin());
                std::uint8_t* parsed{};
                int parsed_size{};
                const int consumed = av_parser_parse2(
                    complete_parser.get(), decoder.codec_context(), &parsed, &parsed_size,
                    reinterpret_cast<const std::uint8_t*>(padded.data()), static_cast<int>(size),
                    static_cast<std::int64_t>(ready_ns), AV_NOPTS_VALUE, 0);
                if (consumed != static_cast<int>(size) || parsed_size != static_cast<int>(size))
                    throw std::runtime_error("complete-AU parser did not emit input immediately");
                decoder.submit_access_unit(
                    {reinterpret_cast<const std::byte*>(parsed), static_cast<std::size_t>(parsed_size)},
                    complete_parser->pts > 0 ? static_cast<std::uint64_t>(complete_parser->pts) : 0U);
            } else {
                decoder.submit_access_unit(au, ready_ns);
            }
            after_inputs.push_back(decoder.statistics().decoded_frames);
        }
        if (after_inputs.empty() || !offsets.eof()) throw std::runtime_error("invalid offset file");
        const auto before_flush = decoder.statistics().decoded_frames;
        if (stream_parser) stream_parser->flush();
        decoder.flush();
        const auto& stats = decoder.statistics();
        std::printf("boundary-probe: {\"mode\":\"%s\",\"input_frames\":%zu,"
                    "\"before_flush\":%llu,\"after_flush\":%llu,\"hardware_frames\":%llu,"
                    "\"decoded_after_inputs\":[", mode.c_str(), after_inputs.size(),
                    static_cast<unsigned long long>(before_flush),
                    static_cast<unsigned long long>(stats.decoded_frames),
                    static_cast<unsigned long long>(stats.hardware_frames));
        for (std::size_t i = 0; i < after_inputs.size(); ++i)
            std::printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(after_inputs[i]));
        std::puts("]}");
        teleop::print_software_metrics();
        return stats.hardware_frames == after_inputs.size() ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "frame-boundary-probe: %s\n", error.what());
        return 1;
    }
}
