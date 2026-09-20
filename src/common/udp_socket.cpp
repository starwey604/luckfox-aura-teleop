#include <teleop/udp_socket.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <teleop/packetizer.hpp>

namespace teleop {
namespace {

// A 1280x1080 H.264 access unit can be split into hundreds of UDP datagrams.
// Keep several frames of those datagrams in each kernel queue.  This avoids a
// transient userspace scheduling delay turning into an incomplete access unit.
constexpr int kSocketBufferBytes = 4 * 1024 * 1024;

int make_udp_socket(int family)
{
    const int fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string{"socket: "} + std::strerror(errno));
    }
    return fd;
}

void configure_socket_buffer(int fd, int option)
{
    // The effective Linux value may be capped by net.core.{r,w}mem_max.  The
    // default is still usable, so do not make platform-specific caps fatal.
    (void)setsockopt(fd, SOL_SOCKET, option, &kSocketBufferBytes, sizeof(kSocketBufferBytes));
}

} // namespace

UdpSender::UdpSender(std::string_view destination, std::uint16_t port)
{
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    const std::string destination_text{destination};
    const int status = getaddrinfo(destination_text.c_str(), service.c_str(), &hints, &result);
    if (status != 0) {
        throw std::runtime_error(std::string{"getaddrinfo: "} + gai_strerror(status));
    }

    fd_ = make_udp_socket(result->ai_family);
    configure_socket_buffer(fd_, SO_SNDBUF);
    std::memcpy(&destination_, result->ai_addr, result->ai_addrlen);
    destination_length_ = static_cast<socklen_t>(result->ai_addrlen);
    freeaddrinfo(result);
}

UdpSender::~UdpSender()
{
    if (fd_ >= 0) {
        close(fd_);
    }
}

bool UdpSender::send_frame(std::uint16_t stream_id, std::uint32_t frame_id,
                           PacketFlags flags, std::uint64_t capture_time_ns,
                           std::span<const std::byte> frame,
                           TransportStatistics& statistics)
{
    const std::uint16_t packet_count = packet_count_for_frame(frame.size());
    if (packet_count == 0U) {
        return false;
    }

    wire_headers_.resize(packet_count);
    iovecs_.resize(static_cast<std::size_t>(packet_count) * 2U);
    messages_.assign(packet_count, mmsghdr{});

    for (std::uint16_t packet_id = 0U; packet_id < packet_count; ++packet_id) {
        const std::size_t payload_size = packet_payload_size(frame.size(), packet_id);
        const std::size_t offset = static_cast<std::size_t>(packet_id) * kMaxPayloadSize;
        PacketHeader header{
            .stream_id = stream_id,
            .frame_id = frame_id,
            .packet_id = packet_id,
            .packet_count = packet_count,
            .frame_size = static_cast<std::uint32_t>(frame.size()),
            .fragment_offset = static_cast<std::uint32_t>(offset),
            .payload_size = static_cast<std::uint16_t>(payload_size),
            .flags = flags,
            .capture_time_ns = capture_time_ns,
        };
        if (!serialize_header(header, wire_headers_[packet_id])) {
            return false;
        }

        iovec& header_iov = iovecs_[static_cast<std::size_t>(packet_id) * 2U];
        header_iov.iov_base = wire_headers_[packet_id].data();
        header_iov.iov_len = wire_headers_[packet_id].size();
        iovec& payload_iov = iovecs_[static_cast<std::size_t>(packet_id) * 2U + 1U];
        payload_iov.iov_base = const_cast<std::byte*>(frame.data() + offset);
        payload_iov.iov_len = payload_size;

        mmsghdr& message = messages_[packet_id];
        message.msg_hdr.msg_name = &destination_;
        message.msg_hdr.msg_namelen = destination_length_;
        message.msg_hdr.msg_iov = &header_iov;
        message.msg_hdr.msg_iovlen = 2U;
    }

    std::size_t first_unsent = 0U;
    while (first_unsent < packet_count) {
        const auto remaining = static_cast<unsigned int>(packet_count - first_unsent);
        const int sent = sendmmsg(fd_, messages_.data() + first_unsent, remaining, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (sent == 0) {
            return false;
        }

        const auto sent_count = static_cast<std::size_t>(sent);
        statistics.datagrams_sent += sent_count;
        for (std::size_t packet_id = first_unsent;
             packet_id < first_unsent + sent_count; ++packet_id) {
            statistics.bytes_sent += messages_[packet_id].msg_len;
        }
        first_unsent += sent_count;
    }
    return true;
}

UdpReceiver::UdpReceiver(std::uint16_t port)
{
    fd_ = make_udp_socket(AF_INET6);
    configure_socket_buffer(fd_, SO_RCVBUF);
    const int disabled = 0;
    (void)setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &disabled, sizeof(disabled));

    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(port);
    if (bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string error = std::string{"bind: "} + std::strerror(errno);
        close(fd_);
        fd_ = -1;
        throw std::runtime_error(error);
    }
}

UdpReceiver::~UdpReceiver()
{
    if (fd_ >= 0) {
        close(fd_);
    }
}

int UdpReceiver::fd() const
{
    return fd_;
}

} // namespace teleop
