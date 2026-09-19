#include <teleop/packetizer.hpp>
#include <teleop/protocol.hpp>

#include <limits>

namespace teleop {

std::uint16_t packet_count_for_frame(std::size_t frame_size)
{
    if (frame_size == 0U) {
        return 0U;
    }

    const std::size_t count = (frame_size + kMaxPayloadSize - 1U) / kMaxPayloadSize;
    if (count > std::numeric_limits<std::uint16_t>::max()) {
        return 0U;
    }
    return static_cast<std::uint16_t>(count);
}

std::size_t packet_payload_size(std::size_t frame_size, std::uint16_t packet_id)
{
    const std::size_t offset = static_cast<std::size_t>(packet_id) * kMaxPayloadSize;
    if (offset >= frame_size) {
        return 0U;
    }
    return std::min(kMaxPayloadSize, frame_size - offset);
}

} // namespace teleop
