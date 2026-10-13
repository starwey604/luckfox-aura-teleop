#include "receive_observer.hpp"
#include <teleop/performance.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <stdexcept>
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
std::uint64_t viewer_thread_cpu_ns() { return clock_ns(CLOCK_THREAD_CPUTIME_ID); }

ReceiveBatch::ReceiveBatch(std::size_t capacity) : capacity_(capacity)
{
    if (capacity == 0U || capacity > kMaximum) throw std::runtime_error("invalid receive batch capacity");
    for (std::size_t index = 0U; index < capacity_; ++index) {
        vectors_[index] = {.iov_base = packets_[index].payload.data(), .iov_len = packets_[index].payload.size()};
        messages_[index].msg_hdr.msg_iov = &vectors_[index];
        messages_[index].msg_hdr.msg_iovlen = 1U;
        messages_[index].msg_hdr.msg_control = controls_[index].data.data();
    }
}

void ReceiveObserver::count_call(int packets, int error)
{
    ++calls_;
    if (packets > 0) {
        ++successful_calls_;
        datagrams_ += static_cast<unsigned int>(packets);
        maximum_batch_ = std::max(maximum_batch_, static_cast<unsigned int>(packets));
    } else if (packets < 0 && (error == EAGAIN || error == EWOULDBLOCK)) ++would_block_;
}

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
    count_call(size < 0 ? -1 : 1, saved_errno);
    if (size < 0) { errno = saved_errno; return {.size = size, .received_ns = received_ns}; }
    const auto realtime_now = timestamp_enabled_ ? clock_ns(CLOCK_REALTIME) : 0U;
    const auto monotonic_now = timestamp_enabled_ ? viewer_monotonic_ns() : received_ns;
    return observe(message, size, received_ns, realtime_now, monotonic_now);
}

int ReceiveObserver::receive_batch(int fd, ReceiveBatch& batch)
{
    batch.count_ = 0U;
    for (std::size_t index = 0U; index < batch.capacity_; ++index) {
        auto& message = batch.messages_[index];
        // Kernel writes these fields. In particular, restore cmsg capacity
        // even when the previous receive returned no ancillary information.
        message.msg_hdr.msg_controllen = batch.controls_[index].data.size();
        message.msg_hdr.msg_flags = 0;
        message.msg_len = 0U;
    }
    const auto begin = performance_time_ns();
    const int count = recvmmsg(fd, batch.messages_.data(), static_cast<unsigned int>(batch.capacity_),
                              MSG_DONTWAIT, nullptr);
    const int saved_errno = errno;
    const auto received_ns = viewer_monotonic_ns();
    record_stage("host.recv_syscall", begin, received_ns);
    count_call(count, saved_errno);
    if (count < 0) { errno = saved_errno; return count; }
    batch.count_ = static_cast<std::size_t>(count);
    const auto realtime_now = timestamp_enabled_ ? clock_ns(CLOCK_REALTIME) : 0U;
    const auto monotonic_now = timestamp_enabled_ ? viewer_monotonic_ns() : received_ns;
    for (std::size_t index = 0U; index < batch.count_; ++index) {
        auto& message = batch.messages_[index];
        batch.packets_[index].result = observe(message.msg_hdr, static_cast<ssize_t>(message.msg_len),
                                              received_ns, realtime_now, monotonic_now);
    }
    return count;
}

ReceivedDatagram ReceiveObserver::observe(msghdr& message, ssize_t size, std::uint64_t received_ns,
                                         std::uint64_t realtime_now, std::uint64_t monotonic_now)
{
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
    std::printf("rx-syscalls: calls=%llu successful_calls=%llu datagrams=%llu max_batch=%u mean_batch=%.6f would_block=%llu\n",
                static_cast<unsigned long long>(calls_), static_cast<unsigned long long>(successful_calls_),
                static_cast<unsigned long long>(datagrams_), maximum_batch_,
                successful_calls_ == 0U ? 0.0 : static_cast<double>(datagrams_) / static_cast<double>(successful_calls_),
                static_cast<unsigned long long>(would_block_));
}

} // namespace teleop
