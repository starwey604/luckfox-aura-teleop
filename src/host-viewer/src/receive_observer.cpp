#include "receive_observer.hpp"
#include <teleop/performance.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/socket.h>

namespace teleop {
namespace {
std::uint64_t clock_ns(clockid_t clock)
{
    timespec value{};
    if (clock_gettime(clock, &value) != 0) return 0U;
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000U +
           static_cast<std::uint64_t>(value.tv_nsec);
}
} // namespace

std::uint64_t viewer_monotonic_ns() { return clock_ns(CLOCK_MONOTONIC); }

ReceiveObserver::ReceiveObserver(int fd)
{
    const int enabled = 1;
    overflow_enabled_ = setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &enabled, sizeof(enabled)) == 0;
    if (software_metrics_enabled()) {
        timestamp_enabled_ = setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &enabled, sizeof(enabled)) == 0;
    }
}

ReceivedDatagram ReceiveObserver::receive(int fd, std::span<std::byte> buffer)
{
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(timespec)) + CMSG_SPACE(sizeof(std::uint32_t))> control{};
    iovec payload{.iov_base = buffer.data(), .iov_len = buffer.size()};
    msghdr message{};
    message.msg_iov = &payload;
    message.msg_iovlen = 1U;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    const auto begin = performance_time_ns();
    const ssize_t size = recvmsg(fd, &message, MSG_DONTWAIT);
    const int saved_errno = errno;
    const auto received_ns = viewer_monotonic_ns();
    record_stage("host.recv_syscall", begin, received_ns);
    if (size < 0) { errno = saved_errno; return {.size = size, .received_ns = received_ns}; }
    const bool truncated = (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
    if (truncated) ++truncated_;
    std::uint64_t realtime_rx = 0U;
    for (cmsghdr* item = CMSG_FIRSTHDR(&message); item != nullptr;
         item = CMSG_NXTHDR(&message, item)) {
        if (item->cmsg_level != SOL_SOCKET) continue;
        if (item->cmsg_type == SO_RXQ_OVFL && item->cmsg_len >= CMSG_LEN(sizeof(std::uint32_t))) {
            std::uint32_t count = 0U;
            std::memcpy(&count, CMSG_DATA(item), sizeof(count));
            overflows_ += static_cast<std::uint32_t>(count - last_overflow_);
            last_overflow_ = count;
        }
        if (item->cmsg_type == SO_TIMESTAMPNS && item->cmsg_len >= CMSG_LEN(sizeof(timespec))) {
            timespec stamp{};
            std::memcpy(&stamp, CMSG_DATA(item), sizeof(stamp));
            if (stamp.tv_sec >= 0 && stamp.tv_nsec >= 0 && stamp.tv_nsec < 1'000'000'000L) {
                realtime_rx = static_cast<std::uint64_t>(stamp.tv_sec) * 1'000'000'000U +
                              static_cast<std::uint64_t>(stamp.tv_nsec);
            }
        }
    }
    if (realtime_rx != 0U) {
        ++timestamps_;
        const auto realtime_now = clock_ns(CLOCK_REALTIME);
        const auto monotonic_now = viewer_monotonic_ns();
        const auto offset = static_cast<std::int64_t>(realtime_now) - static_cast<std::int64_t>(monotonic_now);
        const auto delta = offset - realtime_offset_;
        const bool discontinuity = calibrated_ && (delta > 1'000'000 || delta < -1'000'000);
        calibrated_ = true;
        realtime_offset_ = offset;
        if (discontinuity) realtime_valid_after_ = realtime_now;
        if (discontinuity || realtime_now < realtime_rx || realtime_rx < realtime_valid_after_) {
            ++clock_discontinuities_;
        } else {
            const auto age = realtime_now - realtime_rx;
            if (age <= monotonic_now) {
                if (age > max_age_ns_) max_age_ns_ = age;
                // The duration was measured entirely in REALTIME. Never
                // subtract a raw RX timestamp from a MONOTONIC timestamp.
                record_stage("host.kernel_rx_to_user", monotonic_now - age, monotonic_now);
            }
        }
    }
    return {.size = size, .received_ns = received_ns, .truncated = truncated};
}

void ReceiveObserver::print() const
{
    std::printf("rx-observer: timestamp=%s overflow=%s kernel_timestamps=%llu rxq_overflow=%llu truncated=%llu clock_discontinuities=%llu kernel_rx_age_max_ms=%.6f\n",
                timestamp_enabled_ ? "realtime-software" : "disabled", overflow_enabled_ ? "enabled" : "unavailable",
                static_cast<unsigned long long>(timestamps_), static_cast<unsigned long long>(overflows_),
                static_cast<unsigned long long>(truncated_), static_cast<unsigned long long>(clock_discontinuities_),
                static_cast<double>(max_age_ns_) / 1'000'000.0);
}

} // namespace teleop
