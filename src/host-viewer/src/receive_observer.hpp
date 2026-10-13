#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/types.h>

namespace teleop {

std::uint64_t viewer_monotonic_ns();

struct ReceivedDatagram {
    ssize_t size{};
    std::uint64_t received_ns{};
    bool truncated{};
};

// Owned by the receive thread. Software RX timestamps are CLOCK_REALTIME;
// only same-clock durations are mapped onto the local MONOTONIC timeline.
class ReceiveObserver {
public:
    explicit ReceiveObserver(int fd);
    ReceivedDatagram receive(int fd, std::span<std::byte> buffer);
    void print() const;

private:
    bool timestamp_enabled_{};
    bool overflow_enabled_{};
    bool calibrated_{};
    std::int64_t realtime_offset_{};
    std::uint32_t last_overflow_{};
    std::uint64_t overflows_{};
    std::uint64_t timestamps_{};
    std::uint64_t clock_discontinuities_{};
    std::uint64_t truncated_{};
    std::uint64_t max_age_ns_{};
    std::uint64_t realtime_valid_after_{};
};

} // namespace teleop
