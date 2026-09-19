#include <teleop/reassembler.hpp>
#include <teleop/statistics.hpp>
#include <teleop/udp_socket.hpp>

#include <poll.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <span>
#include <string_view>

#include <sys/socket.h>

namespace {

struct Options {
    std::uint16_t port{5000U};
    std::uint32_t max_frames{120U};
    std::uint32_t timeout_ms{5000U};
    bool same_clock{false};
};

void print_usage(const char* program)
{
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "Receive and validate the native Aura UDP protocol. This phase displays\n"
        "transport statistics; FFmpeg decoding and Vulkan presentation follow next.\n"
        "\n"
        "  --port PORT            UDP listen port (default: 5000)\n"
        "  --frames COUNT         Exit after complete frames (default: 120)\n"
        "  --timeout-ms TIMEOUT   Inactivity timeout (default: 5000)\n"
        "  --same-clock           Report source-to-receive latency; only valid when\n"
        "                         sender and viewer share CLOCK_MONOTONIC\n",
        program);
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer& output)
{
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    if (value > std::numeric_limits<Integer>::max()) {
        return false;
    }
    output = static_cast<Integer>(value);
    return true;
}

bool parse_options(int argc, char* argv[], Options& options)
{
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
        if (index + 1 >= argc) {
            return false;
        }
        const std::string_view value{argv[++index]};
        if (option == "--port") {
            if (!parse_integer(value, options.port) || options.port == 0U) {
                return false;
            }
        } else if (option == "--frames") {
            if (!parse_integer(value, options.max_frames) || options.max_frames == 0U) {
                return false;
            }
        } else if (option == "--timeout-ms") {
            if (!parse_integer(value, options.timeout_ms) || options.timeout_ms == 0U) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
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

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    try {
        teleop::UdpReceiver receiver{options.port};
        teleop::FrameReassembler reassembler;
        teleop::TransportStatistics statistics;
        teleop::LatencyStatistics latency;
        std::array<std::byte, teleop::kMaxDatagramSize> datagram{};

        while (statistics.completed_frames < options.max_frames) {
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
            if (frame.has_value() && options.same_clock) {
                const std::uint64_t now = monotonic_time_ns();
                if (now >= frame->capture_time_ns) {
                    latency.add_sample(now - frame->capture_time_ns);
                }
            }
        }

        std::printf(
            "viewer: frames=%llu datagrams=%llu bytes=%llu malformed=%llu duplicates=%llu dropped=%llu\n",
            static_cast<unsigned long long>(statistics.completed_frames),
            static_cast<unsigned long long>(statistics.datagrams_received),
            static_cast<unsigned long long>(statistics.bytes_received),
            static_cast<unsigned long long>(statistics.malformed_datagrams),
            static_cast<unsigned long long>(statistics.duplicate_datagrams),
            static_cast<unsigned long long>(statistics.dropped_frames));
        if (options.same_clock) {
            print_latency(latency);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "host-viewer: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
