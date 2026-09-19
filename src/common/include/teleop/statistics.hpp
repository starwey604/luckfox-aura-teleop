#pragma once

#include <cstdint>
#include <optional>

namespace teleop {

struct TransportStatistics {
    std::uint64_t datagrams_sent{};
    std::uint64_t datagrams_received{};
    std::uint64_t bytes_sent{};
    std::uint64_t bytes_received{};
    std::uint64_t malformed_datagrams{};
    std::uint64_t duplicate_datagrams{};
    std::uint64_t completed_frames{};
    std::uint64_t dropped_frames{};
};

class LatencyStatistics {
public:
    void add_sample(std::uint64_t nanoseconds);

    [[nodiscard]] std::uint64_t sample_count() const;
    [[nodiscard]] std::optional<double> minimum_ms() const;
    [[nodiscard]] std::optional<double> average_ms() const;
    [[nodiscard]] std::optional<double> maximum_ms() const;

private:
    std::uint64_t count_{};
    std::uint64_t minimum_ns_{};
    std::uint64_t maximum_ns_{};
    long double total_ns_{};
};

} // namespace teleop
