#pragma once

#include <cstddef>
#include <cstdint>

namespace teleop {

[[nodiscard]] std::uint16_t packet_count_for_frame(std::size_t frame_size);
[[nodiscard]] std::size_t packet_payload_size(std::size_t frame_size,
                                              std::uint16_t packet_id);

} // namespace teleop
