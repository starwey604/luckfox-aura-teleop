#include <teleop/protocol.hpp>
#include <teleop/reassembler.hpp>
#include <teleop/statistics.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "protocol-test: %s\n", message);
    }
    return condition;
}

std::vector<std::byte> make_datagram(const teleop::PacketHeader& header,
                                     std::span<const std::byte> payload)
{
    teleop::WireHeader wire{};
    if (!teleop::serialize_header(header, wire)) {
        return {};
    }
    std::vector<std::byte> datagram;
    datagram.reserve(wire.size() + payload.size());
    datagram.insert(datagram.end(), wire.begin(), wire.end());
    datagram.insert(datagram.end(), payload.begin(), payload.end());
    return datagram;
}

} // namespace

int main()
{
    constexpr std::array<std::byte, 12U> payload{
        std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
        std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
        std::byte{0x08}, std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b},
    };
    teleop::PacketHeader header{
        .stream_id = 3U,
        .frame_id = 42U,
        .packet_id = 0U,
        .packet_count = 1U,
        .frame_size = static_cast<std::uint32_t>(payload.size()),
        .fragment_offset = 0U,
        .payload_size = static_cast<std::uint16_t>(payload.size()),
        .flags = teleop::PacketFlags::kKeyFrame | teleop::PacketFlags::kCompleteAccessUnit,
        .capture_time_ns = 123'456U,
    };
    const auto datagram = make_datagram(header, payload);
    const auto parsed = teleop::parse_header(datagram);
    if (!check(parsed.has_value(), "valid header did not parse") ||
        !check(parsed->frame_id == header.frame_id, "frame id changed on wire") ||
        !check(parsed->flags == header.flags, "AU flags changed on wire") ||
        !check(parsed->capture_time_ns == header.capture_time_ns, "timestamp changed on wire")) {
        return 1;
    }

    teleop::TransportStatistics statistics;
    teleop::FrameReassembler reassembler;
    const auto frame = reassembler.accept(datagram, statistics);
    if (!check(frame.has_value(), "complete one-packet frame was not reassembled") ||
        !check(frame->payload == std::vector<std::byte>{payload.begin(), payload.end()},
               "reassembled payload changed") ||
        !check(statistics.completed_frames == 1U, "completed frame count is wrong")) {
        return 1;
    }

    auto invalid = datagram;
    invalid[0] = std::byte{0};
    if (!check(!teleop::parse_header(invalid).has_value(), "bad magic parsed as valid")) {
        return 1;
    }

    std::vector<std::byte> large_payload(teleop::kMaxPayloadSize * 2U + 17U);
    for (std::size_t index = 0U; index < large_payload.size(); ++index) {
        large_payload[index] = static_cast<std::byte>(index & 0xffU);
    }
    std::array<std::vector<std::byte>, 3U> fragments;
    for (std::uint16_t packet_id = 0U; packet_id < fragments.size(); ++packet_id) {
        const std::size_t offset = static_cast<std::size_t>(packet_id) * teleop::kMaxPayloadSize;
        const std::size_t payload_size = std::min(teleop::kMaxPayloadSize, large_payload.size() - offset);
        fragments[packet_id] = make_datagram(
            teleop::PacketHeader{
                .stream_id = 3U,
                .frame_id = 43U,
                .packet_id = packet_id,
                .packet_count = static_cast<std::uint16_t>(fragments.size()),
                .frame_size = static_cast<std::uint32_t>(large_payload.size()),
                .fragment_offset = static_cast<std::uint32_t>(offset),
                .payload_size = static_cast<std::uint16_t>(payload_size),
                .flags = teleop::PacketFlags::kCompleteAccessUnit,
                .capture_time_ns = 123'457U,
            },
            std::span<const std::byte>{large_payload}.subspan(offset, payload_size));
    }
    teleop::FrameReassembler reordered_reassembler;
    teleop::TransportStatistics reordered_statistics;
    if (!check(!reordered_reassembler.accept(fragments[1], reordered_statistics, 100U).has_value(),
               "partial reordered frame completed too early") ||
        !check(!reordered_reassembler.accept(fragments[0], reordered_statistics, 200U).has_value(),
               "partial reordered frame completed too early")) {
        return 1;
    }
    const auto reordered_frame = reordered_reassembler.accept(fragments[2], reordered_statistics);
    if (!check(reordered_frame.has_value(), "reordered frame did not complete") ||
        !check(teleop::has_flag(reordered_frame->flags, teleop::PacketFlags::kCompleteAccessUnit),
               "complete AU marker lost after reordering") ||
        !check(reordered_frame->payload == large_payload, "reordered payload changed") ||
        !check(reordered_frame->first_receive_ns == 100U,
               "frame timing did not start at the first accepted reordered fragment")) {
        return 1;
    }
    teleop::FrameReassembler conflicting;
    teleop::TransportStatistics conflicting_statistics;
    if (!check(!conflicting.accept(fragments[0], conflicting_statistics), "partial AU completed")) return 1;
    auto mismatched = fragments[1];
    mismatched[7] = std::byte{0}; // clear complete-AU bit in the wire flags
    if (!check(!conflicting.accept(mismatched, conflicting_statistics), "conflicting AU flags accepted") ||
        !check(conflicting_statistics.malformed_datagrams == 1U, "flag conflict not counted") ||
        !check(!conflicting.accept(fragments[2], conflicting_statistics), "incomplete AU reached decoder")) return 1;
    // A corrected retransmission can complete the frame; no bytes from the
    // rejected fragment were accepted.
    if (!check(conflicting.accept(fragments[1], conflicting_statistics).has_value(),
               "corrected fragment could not complete AU")) return 1;
    teleop::FrameReassembler missing;
    teleop::TransportStatistics missing_statistics;
    (void)missing.accept(fragments[0], missing_statistics);
    if (!check(!missing.accept(fragments[2], missing_statistics), "missing-fragment AU completed")) return 1;
    auto next_header = header; next_header.stream_id = 3U; next_header.frame_id = 44U;
    if (!check(missing.accept(make_datagram(next_header, payload), missing_statistics).has_value(),
               "next complete AU could not recover after loss") ||
        !check(missing_statistics.dropped_frames == 1U, "lost AU not counted")) return 1;
    return 0;
}
