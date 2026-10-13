#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace teleop {

constexpr std::uint32_t kProtocolMagic = 0x41555241U; // "AURA"
constexpr std::uint8_t kProtocolVersion = 1U;
constexpr std::size_t kWireHeaderSize = 40U;
constexpr std::size_t kMaxDatagramSize = 1200U;
constexpr std::size_t kMaxPayloadSize = kMaxDatagramSize - kWireHeaderSize;
constexpr std::size_t kMaxEncodedAccessUnitSize = 2U * 1024U * 1024U;

enum class PacketFlags : std::uint16_t {
    kNone = 0,
    kKeyFrame = 1U << 0U,
    // The entire reassembled payload is one H.264 AU, never a single slice.
    kCompleteAccessUnit = 1U << 1U,
};

constexpr PacketFlags operator|(PacketFlags left, PacketFlags right)
{
    return static_cast<PacketFlags>(static_cast<std::uint16_t>(left) |
                                    static_cast<std::uint16_t>(right));
}

constexpr bool has_flag(PacketFlags value, PacketFlags flag)
{
    return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(flag)) != 0U;
}

struct PacketHeader {
    std::uint16_t stream_id{};
    std::uint32_t frame_id{};
    std::uint16_t packet_id{};
    std::uint16_t packet_count{};
    std::uint32_t frame_size{};
    std::uint32_t fragment_offset{};
    std::uint16_t payload_size{};
    PacketFlags flags{PacketFlags::kNone};
    std::uint64_t capture_time_ns{};
};

using WireHeader = std::array<std::byte, kWireHeaderSize>;

bool serialize_header(const PacketHeader& header, WireHeader& wire);
std::optional<PacketHeader> parse_header(std::span<const std::byte> datagram);

} // namespace teleop
