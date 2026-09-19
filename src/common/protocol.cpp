#include <teleop/protocol.hpp>

#include <limits>

namespace teleop {
namespace {

void write_u16(std::span<std::byte> output, std::size_t offset, std::uint16_t value)
{
    output[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
    output[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

void write_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value)
{
    for (std::size_t index = 0U; index < 4U; ++index) {
        const auto shift = static_cast<unsigned>((3U - index) * 8U);
        output[offset + index] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

void write_u64(std::span<std::byte> output, std::size_t offset, std::uint64_t value)
{
    for (std::size_t index = 0U; index < 8U; ++index) {
        const auto shift = static_cast<unsigned>((7U - index) * 8U);
        output[offset + index] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

std::uint16_t read_u16(std::span<const std::byte> input, std::size_t offset)
{
    return (static_cast<std::uint16_t>(input[offset]) << 8U) |
           static_cast<std::uint16_t>(input[offset + 1U]);
}

std::uint32_t read_u32(std::span<const std::byte> input, std::size_t offset)
{
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
        value = (value << 8U) | static_cast<std::uint32_t>(input[offset + index]);
    }
    return value;
}

std::uint64_t read_u64(std::span<const std::byte> input, std::size_t offset)
{
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(input[offset + index]);
    }
    return value;
}

} // namespace

bool serialize_header(const PacketHeader& header, WireHeader& wire)
{
    if (header.packet_count == 0U || header.packet_id >= header.packet_count ||
        header.payload_size > kMaxPayloadSize || header.frame_size == 0U ||
        header.fragment_offset > header.frame_size ||
        header.payload_size > header.frame_size - header.fragment_offset) {
        return false;
    }

    std::span<std::byte> output{wire};
    write_u32(output, 0U, kProtocolMagic);
    output[4U] = static_cast<std::byte>(kProtocolVersion);
    output[5U] = static_cast<std::byte>(kWireHeaderSize);
    write_u16(output, 6U, static_cast<std::uint16_t>(header.flags));
    write_u16(output, 8U, header.stream_id);
    write_u16(output, 10U, 0U);
    write_u32(output, 12U, header.frame_id);
    write_u16(output, 16U, header.packet_id);
    write_u16(output, 18U, header.packet_count);
    write_u32(output, 20U, header.frame_size);
    write_u32(output, 24U, header.fragment_offset);
    write_u16(output, 28U, header.payload_size);
    write_u16(output, 30U, 0U);
    write_u64(output, 32U, header.capture_time_ns);
    return true;
}

std::optional<PacketHeader> parse_header(std::span<const std::byte> datagram)
{
    if (datagram.size() < kWireHeaderSize || read_u32(datagram, 0U) != kProtocolMagic ||
        static_cast<std::uint8_t>(datagram[4U]) != kProtocolVersion ||
        static_cast<std::size_t>(static_cast<std::uint8_t>(datagram[5U])) != kWireHeaderSize) {
        return std::nullopt;
    }

    PacketHeader header{
        .stream_id = read_u16(datagram, 8U),
        .frame_id = read_u32(datagram, 12U),
        .packet_id = read_u16(datagram, 16U),
        .packet_count = read_u16(datagram, 18U),
        .frame_size = read_u32(datagram, 20U),
        .fragment_offset = read_u32(datagram, 24U),
        .payload_size = read_u16(datagram, 28U),
        .flags = static_cast<PacketFlags>(read_u16(datagram, 6U)),
        .capture_time_ns = read_u64(datagram, 32U),
    };

    const std::size_t expected_packet_count =
        (static_cast<std::size_t>(header.frame_size) + kMaxPayloadSize - 1U) / kMaxPayloadSize;

    if (header.packet_count == 0U || header.packet_id >= header.packet_count ||
        header.packet_count != expected_packet_count ||
        header.payload_size > kMaxPayloadSize || header.frame_size == 0U ||
        header.fragment_offset > header.frame_size ||
        header.payload_size > header.frame_size - header.fragment_offset ||
        datagram.size() != kWireHeaderSize + header.payload_size) {
        return std::nullopt;
    }
    return header;
}

} // namespace teleop
