#pragma once

#include "h264_decoder.hpp"

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
};

class VulkanPresenter final : public DecodedFrameSink {
public:
    explicit VulkanPresenter(const VulkanWindowOptions& options);
    ~VulkanPresenter() override;

    VulkanPresenter(const VulkanPresenter&) = delete;
    VulkanPresenter& operator=(const VulkanPresenter&) = delete;

    void present(const AVFrame& frame) override;

    [[nodiscard]] bool close_requested() const;
    [[nodiscard]] std::uint64_t presented_frames() const;

private:
    void destroy();
    void poll_events();

    SDL_Window* window_{};
    pl_log log_{};
    pl_vk_inst instance_{};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    pl_vulkan vulkan_{};
    pl_swapchain swapchain_{};
    pl_renderer renderer_{};
    bool close_requested_{};
    std::uint64_t presented_frames_{};
};

} // namespace teleop
