#pragma once

#include "h264_decoder.hpp"

#include <array>
#include <cstdint>

struct SDL_Window;

extern "C" {
#include <vulkan/vulkan.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>
#include <libplacebo/vulkan.h>
}

namespace teleop {

struct VulkanWindowOptions {
    int width{1280};
    int height{720};
    int x{0};
    int y{0};
    bool position_specified{false};
    bool borderless{false};
    bool dual_stream{false};
};

class VulkanPresenter final : public DecodedFrameSink {
public:
    explicit VulkanPresenter(const VulkanWindowOptions& options);
    ~VulkanPresenter() override;

    VulkanPresenter(const VulkanPresenter&) = delete;
    VulkanPresenter& operator=(const VulkanPresenter&) = delete;

    void present(const AVFrame& frame) override;
    void present_stream(std::uint16_t stream_id, const AVFrame& frame);
    void update_stream(std::uint16_t stream_id, const AVFrame& frame);
    void render_latest_frames();
    void poll_events();

    [[nodiscard]] bool close_requested() const;
    [[nodiscard]] std::uint64_t presented_frames() const;

private:
    void destroy();
    std::uint64_t test_quit_deadline_ns_{};
    bool test_fail_after_acquire_{};

    SDL_Window* window_{};
    pl_log log_{};
    pl_vk_inst instance_{};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    pl_vulkan vulkan_{};
    pl_swapchain swapchain_{};
    std::array<pl_renderer, 2U> renderers_{};
    bool close_requested_{};
    bool dual_stream_{};
    std::array<AVFrame*, 2U> latest_frames_{};
    // Software diagnostics need persistent upload textures, one set per stream.
    std::array<std::array<pl_tex, 4U>, 2U> software_textures_{};
    std::uint64_t presented_frames_{};
};

} // namespace teleop
