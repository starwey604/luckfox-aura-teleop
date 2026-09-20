#include "h264_decoder.hpp"
#include "vulkan_presenter.hpp"

#include <teleop/reassembler.hpp>
#include <teleop/statistics.hpp>
#include <teleop/udp_socket.hpp>

#include <libavcodec/avcodec.h>

#include <poll.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>

namespace {

struct Options {
    std::uint16_t port{5000U};
    std::uint32_t max_frames{120U};
    std::uint32_t timeout_ms{5000U};
    std::string_view vaapi_device{"/dev/dri/renderD128"};
    std::string_view input_path{};
    bool same_clock{false};
    bool transport_only{false};
    bool headless{false};
    bool software_decode{false};
    teleop::VulkanWindowOptions window{};
};

void print_usage(const char* program)
{
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "Receive complete H.264 access units over Aura UDP and decode them through\n"
        "FFmpeg VAAPI. Configure and run this executable on the CachyOS host only.\n"
        "\n"
        "  --port PORT            UDP listen port (default: 5000)\n"
        "  --frames COUNT         Exit after complete network access units; 0 runs until closed\n"
        "  --timeout-ms TIMEOUT   Network inactivity timeout (default: 5000)\n"
        "  --vaapi-device PATH    DRM render node (default: /dev/dri/renderD128)\n"
        "  --input FILE           Decode a local Annex-B H.264 file, then exit\n"
        "  --window-width PIXELS  Viewer client width (default: 1280)\n"
        "  --window-height PIXELS Viewer client height (default: 720)\n"
        "  --window-x PIXELS      Viewer left position; requires --window-y too\n"
        "  --window-y PIXELS      Viewer top position; requires --window-x too\n"
        "  --borderless           Create the viewer without window decorations\n"
        "  --headless             Decode without creating the SDL3/Vulkan window\n"
        "  --software-decode      Decode in FFmpeg software; diagnostic fallback for VAAPI\n"
        "  --transport-only       Validate UDP reassembly without opening FFmpeg\n"
        "  --same-clock           Report source-to-receive latency; only valid when\n"
        "                         sender and viewer share CLOCK_MONOTONIC\n",
        program);
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer& output)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_options(int argc, char* argv[], Options& options)
{
    bool has_window_x = false;
    bool has_window_y = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--help") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (option == "--same-clock") {
            options.same_clock = true;
            continue;
        }
        if (option == "--transport-only") {
            options.transport_only = true;
            continue;
        }
        if (option == "--headless") {
            options.headless = true;
            continue;
        }
        if (option == "--software-decode") {
            options.software_decode = true;
            continue;
        }
        if (option == "--borderless") {
            options.window.borderless = true;
            continue;
        }
        if (index + 1 >= argc) {
            return false;
        }
        const std::string_view value{argv[++index]};
        if (option == "--port") {
            if (!parse_integer(value, options.port) || options.port == 0U) {
                return false;
            }
        } else if (option == "--frames") {
            if (!parse_integer(value, options.max_frames)) {
                return false;
            }
        } else if (option == "--timeout-ms") {
            if (!parse_integer(value, options.timeout_ms) || options.timeout_ms == 0U) {
                return false;
            }
        } else if (option == "--vaapi-device") {
            options.vaapi_device = value;
        } else if (option == "--input") {
            options.input_path = value;
        } else if (option == "--window-width") {
            if (!parse_integer(value, options.window.width) || options.window.width <= 0) {
                return false;
            }
        } else if (option == "--window-height") {
            if (!parse_integer(value, options.window.height) || options.window.height <= 0) {
                return false;
            }
        } else if (option == "--window-x") {
            if (!parse_integer(value, options.window.x)) {
                return false;
            }
            has_window_x = true;
        } else if (option == "--window-y") {
            if (!parse_integer(value, options.window.y)) {
                return false;
            }
            has_window_y = true;
        } else {
            return false;
        }
    }
    options.window.position_specified = has_window_x && has_window_y;
    return (!options.transport_only || options.input_path.empty()) && has_window_x == has_window_y;
}

std::uint64_t monotonic_time_ns()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void print_latency(const teleop::LatencyStatistics& latency)
{
    if (latency.sample_count() == 0U) {
        return;
    }
    std::printf("source-to-receive-ms: min=%.3f avg=%.3f max=%.3f samples=%llu\n",
                *latency.minimum_ms(), *latency.average_ms(), *latency.maximum_ms(),
                static_cast<unsigned long long>(latency.sample_count()));
}

void print_decoder_statistics(const teleop::DecodeStatistics& statistics)
{
    std::printf("decoder: frames=%llu hardware_frames=%llu\n",
                static_cast<unsigned long long>(statistics.decoded_frames),
                static_cast<unsigned long long>(statistics.hardware_frames));
}

void print_presenter_statistics(const teleop::VulkanPresenter& presenter)
{
    std::printf("presenter: frames=%llu\n",
                static_cast<unsigned long long>(presenter.presented_frames()));
}

std::unique_ptr<teleop::VulkanPresenter> make_presenter(const Options& options)
{
    if (options.headless || options.transport_only) {
        return nullptr;
    }
    return std::make_unique<teleop::VulkanPresenter>(options.window);
}

void decode_annex_b_file(std::string_view path, teleop::H264VaapiDecoder& decoder)
{
    std::ifstream file{std::string{path}, std::ios::binary | std::ios::ate};
    if (!file) {
        throw std::runtime_error("could not open H.264 input file: " + std::string{path});
    }
    const std::streamsize input_size = file.tellg();
    if (input_size <= 0 || input_size > std::numeric_limits<int>::max()) {
        throw std::runtime_error("H.264 input file is empty or too large for the parser");
    }
    file.seekg(0);

    std::vector<std::byte> input(
        static_cast<std::size_t>(input_size) + AV_INPUT_BUFFER_PADDING_SIZE, std::byte{0});
    if (!file.read(reinterpret_cast<char*>(input.data()), input_size)) {
        throw std::runtime_error("could not read H.264 input file");
    }

    teleop::H264AnnexBParser parser{decoder};
    constexpr std::size_t kInputChunkBytes = 257U;
    const std::span<const std::byte> stream{input.data(), static_cast<std::size_t>(input_size)};
    for (std::size_t offset = 0U; offset < stream.size(); offset += kInputChunkBytes) {
        const std::size_t chunk_size = std::min(kInputChunkBytes, stream.size() - offset);
        parser.submit_chunk(stream.subspan(offset, chunk_size));
    }
    parser.flush();
    decoder.flush();
}

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    try {
        auto presenter = make_presenter(options);
        if (!options.input_path.empty()) {
            teleop::H264VaapiDecoder decoder{options.vaapi_device, presenter.get(),
                                             !options.software_decode};
            decode_annex_b_file(options.input_path, decoder);
            print_decoder_statistics(decoder.statistics());
            if (presenter != nullptr) {
                print_presenter_statistics(*presenter);
            }
            return decoder.statistics().decoded_frames == 0U ? EXIT_FAILURE : EXIT_SUCCESS;
        }

        teleop::UdpReceiver receiver{options.port};
        teleop::FrameReassembler reassembler;
        teleop::TransportStatistics statistics;
        teleop::LatencyStatistics latency;
        std::array<std::byte, teleop::kMaxDatagramSize> datagram{};
        std::optional<teleop::H264VaapiDecoder> decoder;
        std::optional<teleop::H264AnnexBParser> parser;
        if (!options.transport_only) {
            decoder.emplace(options.vaapi_device, presenter.get(), !options.software_decode);
            parser.emplace(*decoder);
        }

        while (options.max_frames == 0U || statistics.completed_frames < options.max_frames) {
            pollfd descriptor{.fd = receiver.fd(), .events = POLLIN, .revents = 0};
            const int readiness = poll(&descriptor, 1U, static_cast<int>(options.timeout_ms));
            if (readiness == 0) {
                std::fprintf(stderr, "host-viewer: timeout after %u ms without a datagram\n",
                             options.timeout_ms);
                return EXIT_FAILURE;
            }
            if (readiness < 0) {
                std::perror("host-viewer: poll");
                return EXIT_FAILURE;
            }

            const ssize_t received = recv(receiver.fd(), datagram.data(), datagram.size(), 0);
            if (received < 0) {
                std::perror("host-viewer: recv");
                return EXIT_FAILURE;
            }
            const auto frame = reassembler.accept(
                std::span<const std::byte>{datagram.data(), static_cast<std::size_t>(received)}, statistics);
            if (!frame.has_value()) {
                continue;
            }
            if (decoder.has_value()) {
                parser->submit_chunk(frame->payload);
            }
            if (presenter != nullptr && presenter->close_requested()) {
                break;
            }
            if (options.same_clock) {
                const std::uint64_t now = monotonic_time_ns();
                if (now >= frame->capture_time_ns) {
                    latency.add_sample(now - frame->capture_time_ns);
                }
            }
        }

        if (decoder.has_value()) {
            parser->flush();
            decoder->flush();
        }
        std::printf(
            "viewer: access_units=%llu datagrams=%llu bytes=%llu malformed=%llu duplicates=%llu dropped=%llu\n",
            static_cast<unsigned long long>(statistics.completed_frames),
            static_cast<unsigned long long>(statistics.datagrams_received),
            static_cast<unsigned long long>(statistics.bytes_received),
            static_cast<unsigned long long>(statistics.malformed_datagrams),
            static_cast<unsigned long long>(statistics.duplicate_datagrams),
            static_cast<unsigned long long>(statistics.dropped_frames));
        if (decoder.has_value()) {
            print_decoder_statistics(decoder->statistics());
        }
        if (presenter != nullptr) {
            print_presenter_statistics(*presenter);
        }
        if (options.same_clock) {
            print_latency(latency);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "host-viewer: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
