#include <teleop/protocol.hpp>
#include <teleop/statistics.hpp>
#include <teleop/udp_socket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string_view destination{"127.0.0.1"};
    std::uint16_t port{5000U};
    std::uint16_t stream_id{0U};
    std::uint32_t frame_count{120U};
    std::uint32_t frame_bytes{24'000U};
    std::uint32_t frames_per_second{60U};
};

void print_usage(const char* program)
{
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "Temporary synthetic source for the Aura UDP sender. It exercises the\n"
        "same packetization and sendmmsg path that will receive RK MPI VENC output.\n"
        "\n"
        "  --destination ADDRESS  UDP destination (default: 127.0.0.1)\n"
        "  --port PORT            UDP destination port (default: 5000)\n"
        "  --stream-id ID         Stream identifier (default: 0)\n"
        "  --frames COUNT         Frames to transmit (default: 120)\n"
        "  --frame-bytes BYTES    Synthetic access-unit size (default: 24000)\n"
        "  --fps RATE             Transmit rate (default: 60)\n",
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
        if (index + 1 >= argc) {
            return false;
        }
        const std::string_view value{argv[++index]};
        if (option == "--destination") {
            options.destination = value;
        } else if (option == "--port") {
            if (!parse_integer(value, options.port) || options.port == 0U) {
                return false;
            }
        } else if (option == "--stream-id") {
            if (!parse_integer(value, options.stream_id)) {
                return false;
            }
        } else if (option == "--frames") {
            if (!parse_integer(value, options.frame_count) || options.frame_count == 0U) {
                return false;
            }
        } else if (option == "--frame-bytes") {
            if (!parse_integer(value, options.frame_bytes) || options.frame_bytes == 0U) {
                return false;
            }
        } else if (option == "--fps") {
            if (!parse_integer(value, options.frames_per_second) || options.frames_per_second == 0U) {
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

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    try {
        teleop::UdpSender sender{options.destination, options.port};
        teleop::TransportStatistics statistics;
        std::vector<std::byte> frame(options.frame_bytes);
        const auto interval = std::chrono::nanoseconds{1'000'000'000U / options.frames_per_second};
        auto deadline = std::chrono::steady_clock::now();

        for (std::uint32_t frame_id = 0U; frame_id < options.frame_count; ++frame_id) {
            for (std::size_t offset = 0U; offset < frame.size(); ++offset) {
                frame[offset] = static_cast<std::byte>((frame_id + offset) & 0xffU);
            }
            const auto flags = frame_id == 0U ? teleop::PacketFlags::kKeyFrame
                                              : teleop::PacketFlags::kNone;
            if (!sender.send_frame(options.stream_id, frame_id, flags, monotonic_time_ns(), frame,
                                   statistics)) {
                std::fprintf(stderr, "frame %u was not completely queued for UDP transmission\n", frame_id);
            }
            deadline += interval;
            std::this_thread::sleep_until(deadline);
        }

        std::printf("sender: frames=%u datagrams=%llu bytes=%llu\n", options.frame_count,
                    static_cast<unsigned long long>(statistics.datagrams_sent),
                    static_cast<unsigned long long>(statistics.bytes_sent));
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "aura-sender: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
