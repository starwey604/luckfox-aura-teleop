#include "h264_decoder.hpp"
#include "vulkan_presenter.hpp"
#include "viewer_pipeline.hpp"
#include "receive_observer.hpp"

#include <teleop/reassembler.hpp>
#include <teleop/statistics.hpp>
#include <teleop/udp_socket.hpp>
#include <teleop/performance.hpp>

#include <libavcodec/avcodec.h>

#include <poll.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <thread>
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
    bool dual_stream{false};
    bool force_stream_parser{false};
    bool async_pipeline{true};
    std::size_t queue_capacity{4U};
    std::uint32_t queue_report_ms{};
    std::uint32_t diagnostic_present_delay_ms{};
    std::uint32_t diagnostic_decode_delay_ms{};
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
        "  --dual-stream          Receive stream IDs 0 and 1, stacked top to bottom\n"
        "  --pipeline async|sync  Independent RX/decoder/display (default: async)\n"
        "  --decode-queue-capacity N  Per-stream encoded AU limit (default: 4; 1..64)\n"
        "  --queue-report-ms N    Live async queue snapshots; 0 disables (default: 0)\n"
        "  --diagnostic-present-delay-ms N  Test-only display blocking (0..1000)\n"
        "  --diagnostic-decode-delay-ms N   Test-only worker blocking (0..1000; async)\n"
        "  --force-stream-parser  Diagnostic: use Annex-B lookahead even for marked complete AUs\n"
        "  --transport-only       Validate UDP reassembly without opening FFmpeg\n"
        "  --same-clock           Report source-to-receive latency; only valid when\n"
        "                         sender and viewer share CLOCK_MONOTONIC\n"
        "  TELEOP_METRICS=1       Opt-in local stage timings and socket buffer diagnostics\n",
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
        if (option == "--dual-stream") {
            options.dual_stream = true;
            continue;
        }
        if (option == "--force-stream-parser") {
            options.force_stream_parser = true;
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
        } else if (option == "--pipeline") {
            if (value != "async" && value != "sync") return false;
            options.async_pipeline = value == "async";
        } else if (option == "--decode-queue-capacity") {
            if (!parse_integer(value, options.queue_capacity) || options.queue_capacity == 0U || options.queue_capacity > 64U) return false;
        } else if (option == "--queue-report-ms") {
            if (!parse_integer(value, options.queue_report_ms) || options.queue_report_ms > 60000U) return false;
        } else if (option == "--diagnostic-present-delay-ms") {
            if (!parse_integer(value, options.diagnostic_present_delay_ms) || options.diagnostic_present_delay_ms > 1000U) return false;
        } else if (option == "--diagnostic-decode-delay-ms") {
            if (!parse_integer(value, options.diagnostic_decode_delay_ms) || options.diagnostic_decode_delay_ms > 1000U) return false;
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
    return (!options.transport_only || options.input_path.empty()) && has_window_x == has_window_y &&
           (options.async_pipeline || (options.diagnostic_decode_delay_ms == 0U && options.queue_report_ms == 0U));
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
    auto window = options.window;
    window.dual_stream = options.dual_stream;
    return std::make_unique<teleop::VulkanPresenter>(window);
}

class StreamPresenterSink final : public teleop::DecodedFrameSink {
public:
    StreamPresenterSink(teleop::VulkanPresenter& presenter, std::uint16_t stream_id, std::uint32_t delay_ms)
        : presenter_(presenter), stream_id_(stream_id), delay_ms_(delay_ms)
    {
    }

    void present(const AVFrame& frame) override
    {
        if (delay_ms_ != 0U) std::this_thread::sleep_for(std::chrono::milliseconds{delay_ms_});
        presenter_.present_stream(stream_id_, frame);
    }

private:
    teleop::VulkanPresenter& presenter_;
    std::uint16_t stream_id_{};
    std::uint32_t delay_ms_{};
};

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

        if (options.async_pipeline) {
            teleop::run_viewer_pipeline({
                .port = options.port, .max_frames = options.max_frames, .timeout_ms = options.timeout_ms,
                .stream_count = options.dual_stream ? 2U : 1U, .queue_capacity = options.queue_capacity,
                .queue_report_ms = options.queue_report_ms,
                .vaapi_device = options.vaapi_device, .software_decode = options.software_decode,
                .transport_only = options.transport_only, .force_stream_parser = options.force_stream_parser,
                .same_clock = options.same_clock,
                .diagnostic_present_delay_ms = options.diagnostic_present_delay_ms,
                .diagnostic_decode_delay_ms = options.diagnostic_decode_delay_ms}, presenter.get());
            return EXIT_SUCCESS;
        }

        teleop::UdpReceiver receiver{options.port};
        teleop::ReceiveObserver observer{receiver.fd()};
        std::array<teleop::FrameReassembler, 2U> reassemblers;
        teleop::TransportStatistics statistics;
        teleop::LatencyStatistics latency;
        std::array<std::byte, teleop::kMaxDatagramSize> datagram{};
        constexpr std::size_t kMaximumStreams = 2U;
        const std::size_t stream_count = options.dual_stream ? kMaximumStreams : 1U;
        std::array<std::optional<teleop::H264VaapiDecoder>, kMaximumStreams> decoders;
        std::array<std::optional<teleop::H264AnnexBParser>, kMaximumStreams> parsers;
        std::array<std::unique_ptr<StreamPresenterSink>, kMaximumStreams> sinks;
        std::array<std::optional<bool>, kMaximumStreams> complete_au_contracts;
        if (!options.transport_only) {
            for (std::size_t stream = 0U; stream < stream_count; ++stream) {
                if (presenter != nullptr) {
                    sinks[stream] = std::make_unique<StreamPresenterSink>(
                        *presenter, static_cast<std::uint16_t>(stream), options.diagnostic_present_delay_ms);
                }
                decoders[stream].emplace(options.vaapi_device, sinks[stream].get(),
                                         !options.software_decode, static_cast<std::uint16_t>(stream));
                parsers[stream].emplace(*decoders[stream]);
            }
        }

        std::array<std::uint64_t, kMaximumStreams> previous_ready_ns{};
        std::array<std::string, kMaximumStreams> metric_prefixes{"host.stream0.", "host.stream1."};

        while (options.max_frames == 0U || statistics.completed_frames < options.max_frames) {
            pollfd descriptor{.fd = receiver.fd(), .events = POLLIN, .revents = 0};
            const int readiness = poll(&descriptor, 1U, static_cast<int>(std::min(options.timeout_ms, static_cast<std::uint32_t>(std::numeric_limits<int>::max()))));
            if (readiness == 0) {
                std::fprintf(stderr, "host-viewer: timeout after %u ms without a datagram\n",
                             options.timeout_ms);
                return EXIT_FAILURE;
            }
            if (readiness < 0) {
                std::perror("host-viewer: poll");
                return EXIT_FAILURE;
            }

            const auto received_datagram = observer.receive(receiver.fd(), datagram);
            const ssize_t received = received_datagram.size;
            const auto received_ns = received_datagram.received_ns;
            if (received < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                std::perror("host-viewer: recvmsg");
                return EXIT_FAILURE;
            }
            if (received_datagram.truncated) { ++statistics.malformed_datagrams; continue; }
            const std::span<const std::byte> datagram_view{datagram.data(),
                                                            static_cast<std::size_t>(received)};
            const auto header = teleop::parse_header(datagram_view);
            if (!header.has_value() || header->stream_id >= stream_count) {
                ++statistics.malformed_datagrams;
                continue;
            }
            const auto frame = reassemblers[header->stream_id].accept(datagram_view, statistics, received_ns);
            if (!frame.has_value()) {
                continue;
            }
            const auto ready_ns = teleop::performance_time_ns();
            const auto& prefix = metric_prefixes[frame->stream_id];
            if (ready_ns != 0U) {
                teleop::record_stage(prefix + "reassembly", frame->first_receive_ns, ready_ns);
                teleop::record_stage(prefix + "au_interval", previous_ready_ns[frame->stream_id], ready_ns);
            }
            previous_ready_ns[frame->stream_id] = ready_ns;
            if (decoders[frame->stream_id].has_value()) {
                const auto parse_begin = teleop::performance_time_ns();
                const bool complete_au = teleop::has_flag(frame->flags, teleop::PacketFlags::kCompleteAccessUnit);
                auto& contract = complete_au_contracts[frame->stream_id];
                if (contract.has_value() && *contract != complete_au) {
                    throw std::runtime_error("H.264 AU contract changed midstream; restart viewer");
                }
                if (!contract.has_value()) {
                    contract = complete_au;
                    std::printf("viewer stream %u: input=%s\n", frame->stream_id,
                                complete_au && !options.force_stream_parser ? "complete-au" : "annex-b-stream");
                }
                if (complete_au && !options.force_stream_parser) {
                    decoders[frame->stream_id]->submit_access_unit(frame->payload, ready_ns);
                } else {
                    parsers[frame->stream_id]->submit_chunk(frame->payload, ready_ns);
                }
                if (parse_begin != 0U) {
                    teleop::record_stage(prefix + "parser_decode_call", parse_begin, teleop::performance_time_ns());
                }
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

        for (std::size_t stream = 0U; stream < stream_count; ++stream) {
            if (decoders[stream].has_value()) {
                parsers[stream]->flush();
                decoders[stream]->flush();
            }
        }
        std::printf(
            "viewer: access_units=%llu datagrams=%llu bytes=%llu malformed=%llu duplicates=%llu dropped=%llu\n",
            static_cast<unsigned long long>(statistics.completed_frames),
            static_cast<unsigned long long>(statistics.datagrams_received),
            static_cast<unsigned long long>(statistics.bytes_received),
            static_cast<unsigned long long>(statistics.malformed_datagrams),
            static_cast<unsigned long long>(statistics.duplicate_datagrams),
            static_cast<unsigned long long>(statistics.dropped_frames));
        for (std::size_t stream = 0U; stream < stream_count; ++stream) {
            if (decoders[stream].has_value()) {
                std::printf("viewer stream %zu: ", stream);
                print_decoder_statistics(decoders[stream]->statistics());
            }
        }
        if (presenter != nullptr) {
            print_presenter_statistics(*presenter);
        }
        if (options.same_clock) {
            print_latency(latency);
        }
        std::printf("pipeline: mode=sync\n");
        observer.print();
        teleop::print_software_metrics();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "host-viewer: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
