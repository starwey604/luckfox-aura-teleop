#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <ctime>
#include <span>
#include <sys/socket.h>
#include <sys/types.h>
#include <teleop/protocol.hpp>

namespace teleop {

std::uint64_t viewer_monotonic_ns();
std::uint64_t viewer_thread_cpu_ns();

struct ReceivedDatagram {
    ssize_t size{};
    std::uint64_t received_ns{};
    bool truncated{};
};

struct ReceivedPacket {
    std::array<std::byte, kMaxDatagramSize> payload{};
    ReceivedDatagram result;
};

// Stable storage: mmsghdr/iovec pointers refer to this object's packet buffers.
class ReceiveBatch {
public:
    static constexpr std::size_t kMaximum = 32U;
    explicit ReceiveBatch(std::size_t capacity);
    ReceiveBatch(const ReceiveBatch&) = delete;
    ReceiveBatch& operator=(const ReceiveBatch&) = delete;
    std::span<const ReceivedPacket> packets() const { return {packets_.data(), count_}; }

private:
    friend class ReceiveObserver;
    struct Control {
        alignas(cmsghdr) std::array<std::byte,
            CMSG_SPACE(sizeof(timespec)) + CMSG_SPACE(sizeof(std::uint32_t))> data{};
    };
    std::size_t capacity_;
    std::size_t count_{};
    std::array<ReceivedPacket, kMaximum> packets_{};
    std::array<iovec, kMaximum> vectors_{};
    std::array<mmsghdr, kMaximum> messages_{};
    std::array<Control, kMaximum> controls_{};
};

// Owned by the receive thread. Software RX timestamps are CLOCK_REALTIME;
// only same-clock durations are mapped onto the local MONOTONIC timeline.
class ReceiveObserver {
public:
    explicit ReceiveObserver(int fd);
    ReceivedDatagram receive(int fd, std::span<std::byte> buffer);
    int receive_batch(int fd, ReceiveBatch& batch);
    void print() const;
    std::uint64_t kernel_timestamps() const { return timestamps_; }
    std::uint64_t overflow_datagrams() const { return overflows_; }
    std::uint64_t truncated_datagrams() const { return truncated_; }

private:
    ReceivedDatagram observe(msghdr& message, ssize_t size, std::uint64_t received_ns,
                            std::uint64_t realtime_now, std::uint64_t monotonic_now);
    void count_call(int packets, int error);
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
    std::uint64_t calls_{};
    std::uint64_t successful_calls_{};
    std::uint64_t datagrams_{};
    std::uint64_t would_block_{};
    unsigned int maximum_batch_{};
};

} // namespace teleop
