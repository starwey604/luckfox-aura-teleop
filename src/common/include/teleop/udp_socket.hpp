#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <sys/socket.h>

#include <teleop/protocol.hpp>
#include <teleop/statistics.hpp>

namespace teleop {

class UdpSender {
public:
    UdpSender(std::string_view destination, std::uint16_t port);
    ~UdpSender();

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    [[nodiscard]] bool send_frame(std::uint16_t stream_id, std::uint32_t frame_id,
                                  PacketFlags flags, std::uint64_t capture_time_ns,
                                  std::span<const std::byte> frame,
                                  TransportStatistics& statistics);

private:
    int fd_{-1};
    sockaddr_storage destination_{};
    socklen_t destination_length_{};
    std::vector<WireHeader> wire_headers_;
    std::vector<iovec> iovecs_;
    std::vector<mmsghdr> messages_;
};

class UdpReceiver {
public:
    explicit UdpReceiver(std::uint16_t port);
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    [[nodiscard]] int fd() const;

private:
    int fd_{-1};
};

} // namespace teleop
