#include "receive_observer.hpp"

#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* reason)
{
    if (!condition) throw std::runtime_error(reason);
}
struct Socket {
    int fd{socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    Socket() { check(fd >= 0, "socket failed"); }
    ~Socket() { close(fd); }
};
struct Link {
    Socket receiver, sender;
    sockaddr_in address{.sin_family = AF_INET, .sin_port = 0,
                        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}, .sin_zero = {}};
    Link()
    {
        check(bind(receiver.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind failed");
        socklen_t length = sizeof(address);
        check(getsockname(receiver.fd, reinterpret_cast<sockaddr*>(&address), &length) == 0, "getsockname failed");
    }
    void send(std::span<const std::byte> data)
    {
        check(sendto(sender.fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)) == static_cast<ssize_t>(data.size()), "sendto failed");
    }
};
void batch_reuse()
{
    Link link;
    teleop::ReceiveObserver observer{link.receiver.fd};
    teleop::ReceiveBatch batch{32};
    auto begin = teleop::viewer_monotonic_ns();
    check(observer.receive_batch(link.receiver.fd, batch) == -1 && errno == EAGAIN, "empty batch must return EAGAIN");
    check(teleop::viewer_monotonic_ns() - begin < 100'000'000U, "empty batch blocked");
    std::uint64_t expected_timestamps = 0;
    for (unsigned count : {1U, 3U, 32U, 7U}) {
        for (unsigned sequence = 0; sequence < count; ++sequence) {
            std::array<std::byte, 4> payload{};
            std::memcpy(payload.data(), &sequence, sizeof(sequence));
            link.send(payload);
        }
        begin = teleop::viewer_monotonic_ns();
        check(observer.receive_batch(link.receiver.fd, batch) == static_cast<int>(count), "partial/full batch count wrong");
        check(teleop::viewer_monotonic_ns() - begin < 100'000'000U, "partial batch waited to fill");
        unsigned sequence = 0;
        for (const auto& packet : batch.packets()) {
            unsigned value = 0;
            std::memcpy(&value, packet.payload.data(), sizeof(value));
            check(value == sequence++ && packet.result.size == 4 && !packet.result.truncated,
                  "batch payload/ordering corrupted");
        }
        expected_timestamps += count;
        check(observer.kernel_timestamps() == expected_timestamps, "per-packet cmsg timestamp lost on reuse");
    }
    const std::vector<std::byte> oversized(1400, std::byte{1});
    link.send(oversized);
    link.send({});
    check(observer.receive_batch(link.receiver.fd, batch) == 2, "truncation batch count wrong");
    check(batch.packets()[0].result.truncated && batch.packets()[1].result.size == 0,
          "truncation or empty UDP packet handling wrong");
    check(observer.truncated_datagrams() == 1 && observer.kernel_timestamps() == expected_timestamps + 2,
          "truncation cmsg accounting wrong");
    check(observer.receive_batch(link.receiver.fd, batch) == -1 && errno == EAGAIN && batch.packets().empty(),
          "EAGAIN retained stale packets");
    observer.print();
}
void overflow_cmsg()
{
    Link link;
    const int small_buffer = 4096;
    check(setsockopt(link.receiver.fd, SOL_SOCKET, SO_RCVBUF, &small_buffer, sizeof(small_buffer)) == 0,
          "small receive buffer failed");
    teleop::ReceiveObserver observer{link.receiver.fd};
    teleop::ReceiveBatch batch{8};
    const std::vector<std::byte> payload(1200, std::byte{2});
    for (unsigned index = 0; index < 256; ++index) link.send(payload);
    while (observer.receive_batch(link.receiver.fd, batch) > 0) {}
    check(errno == EAGAIN, "overflow drain error");
    // The overflow counter is attached to a subsequent successfully queued packet.
    link.send(payload);
    check(observer.receive_batch(link.receiver.fd, batch) == 1, "post-overflow receive failed");
    check(observer.overflow_datagrams() > 0, "SO_RXQ_OVFL cmsg lost");
    observer.print();
}
} // namespace

int main()
{
    try { batch_reuse(); overflow_cmsg(); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "receive-observer-test: %s\n", error.what()); return 1; }
}
