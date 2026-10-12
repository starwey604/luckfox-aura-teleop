#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <teleop/protocol.hpp>
#include <teleop/statistics.hpp>

namespace teleop {

struct CompletedFrame {
    std::uint16_t stream_id{};
    std::uint32_t frame_id{};
    PacketFlags flags{PacketFlags::kNone};
    std::uint64_t capture_time_ns{};
    std::vector<std::byte> payload;
    std::uint64_t first_receive_ns{}; // host-local; not part of the wire protocol
};

class FrameReassembler {
public:
    explicit FrameReassembler(std::size_t maximum_frame_size = kMaxEncodedAccessUnitSize);

    [[nodiscard]] std::optional<CompletedFrame> accept(
        std::span<const std::byte> datagram, TransportStatistics& statistics,
        std::uint64_t receive_ns = 0U);

private:
    void reset();
    bool begin_frame(const PacketHeader& header, std::uint64_t receive_ns);

    const std::size_t maximum_frame_size_;
    bool active_{};
    PacketHeader active_header_{};
    std::uint16_t received_packet_count_{};
    std::vector<bool> received_packets_;
    std::vector<std::byte> buffer_;
    std::uint64_t first_receive_ns_{};
};

} // namespace teleop
