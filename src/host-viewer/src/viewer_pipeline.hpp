#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace teleop {
class VulkanPresenter;

struct ViewerPipelineOptions {
    std::uint16_t port{5000U};
    std::uint32_t max_frames{120U};
    std::uint32_t timeout_ms{5000U};
    std::size_t stream_count{1U};
    std::size_t queue_capacity{4U};
    std::uint32_t queue_report_ms{};
    std::string_view vaapi_device{"/dev/dri/renderD128"};
    bool software_decode{};
    bool transport_only{};
    bool force_stream_parser{};
    bool same_clock{};
    std::uint32_t diagnostic_present_delay_ms{};
    std::uint32_t diagnostic_decode_delay_ms{};
};

// SDL/Vulkan remain on the caller's main thread; all threads are joined before
// this returns or rethrows a worker error. Encoded queue overflow is fatal,
// rather than silently discarding H.264 reference pictures.
void run_viewer_pipeline(const ViewerPipelineOptions& options, VulkanPresenter* presenter);
} // namespace teleop
