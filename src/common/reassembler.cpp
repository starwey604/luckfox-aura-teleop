#include <teleop/reassembler.hpp>

#include <algorithm>
#include <cstring>

namespace teleop {
namespace {

bool frame_id_is_newer(std::uint32_t candidate, std::uint32_t current)
{
    const std::uint32_t distance = candidate - current;
    return distance != 0U && distance < 0x80000000U;
}

} // namespace

FrameReassembler::FrameReassembler(std::size_t maximum_frame_size)
    : maximum_frame_size_(maximum_frame_size)
{
}

void FrameReassembler::reset()
{
    active_ = false;
    received_packet_count_ = 0U;
    received_packets_.clear();
    buffer_.clear();
}

bool FrameReassembler::begin_frame(const PacketHeader& header, std::uint64_t receive_ns)
{
    if (header.frame_size > maximum_frame_size_) {
        return false;
    }
    active_ = true;
    active_header_ = header;
    first_receive_ns_ = receive_ns;
    received_packet_count_ = 0U;
    received_packets_.assign(header.packet_count, false);
    buffer_.assign(header.frame_size, std::byte{0});
    return true;
}

std::optional<CompletedFrame> FrameReassembler::accept(
    std::span<const std::byte> datagram, TransportStatistics& statistics, std::uint64_t receive_ns)
{
    const auto header = parse_header(datagram);
    if (!header.has_value()) {
        ++statistics.malformed_datagrams;
        return std::nullopt;
    }

    ++statistics.datagrams_received;
    statistics.bytes_received += datagram.size();

    if (!active_ || header->stream_id != active_header_.stream_id ||
        header->frame_id != active_header_.frame_id) {
        if (active_ && header->stream_id == active_header_.stream_id &&
            !frame_id_is_newer(header->frame_id, active_header_.frame_id)) {
            return std::nullopt;
        }
        if (active_) {
            ++statistics.dropped_frames;
        }
        if (!begin_frame(*header, receive_ns)) {
            ++statistics.malformed_datagrams;
            return std::nullopt;
        }
    }

    if (header->packet_count != active_header_.packet_count ||
        header->frame_size != active_header_.frame_size ||
        header->capture_time_ns != active_header_.capture_time_ns) {
        ++statistics.malformed_datagrams;
        return std::nullopt;
    }

    const std::size_t expected_offset = static_cast<std::size_t>(header->packet_id) * kMaxPayloadSize;
    if (expected_offset >= header->frame_size) {
        ++statistics.malformed_datagrams;
        return std::nullopt;
    }
    const std::size_t expected_payload_size =
        std::min(kMaxPayloadSize, static_cast<std::size_t>(header->frame_size) - expected_offset);
    if (header->fragment_offset != expected_offset || header->payload_size != expected_payload_size) {
        ++statistics.malformed_datagrams;
        return std::nullopt;
    }

    if (received_packets_[header->packet_id]) {
        ++statistics.duplicate_datagrams;
        return std::nullopt;
    }

    const auto payload = datagram.subspan(kWireHeaderSize);
    std::memcpy(buffer_.data() + header->fragment_offset, payload.data(), payload.size());
    received_packets_[header->packet_id] = true;
    ++received_packet_count_;

    if (received_packet_count_ != active_header_.packet_count) {
        return std::nullopt;
    }

    CompletedFrame completed{
        .stream_id = active_header_.stream_id,
        .frame_id = active_header_.frame_id,
        .flags = active_header_.flags,
        .capture_time_ns = active_header_.capture_time_ns,
        .payload = std::move(buffer_),
        .first_receive_ns = first_receive_ns_,
    };
    reset();
    ++statistics.completed_frames;
    return completed;
}

} // namespace teleop
