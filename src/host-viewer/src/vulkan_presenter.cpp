#include "vulkan_presenter.hpp"

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdexcept>
#include <string>

namespace teleop {
namespace {

[[noreturn]] void throw_sdl_error(const char* operation)
{
    throw std::runtime_error(std::string{operation} + ": " + SDL_GetError());
}

} // namespace

VulkanPresenter::VulkanPresenter(int width, int height)
{
    try {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw_sdl_error("SDL video initialization failed");
        }
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            throw_sdl_error("SDL Vulkan loader initialization failed");
        }
        window_ = SDL_CreateWindow("Luckfox Aura Teleop", width, height,
                                   SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr) {
            throw_sdl_error("SDL window creation failed");
        }

        std::uint32_t extension_count = 0U;
        const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
        if (extensions == nullptr || extension_count == 0U) {
            throw_sdl_error("SDL Vulkan extension query failed");
        }

        const pl_log_params log_params{
            .log_cb = pl_log_simple,
            .log_priv = stderr,
            .log_level = PL_LOG_WARN,
        };
        log_ = pl_log_create(PL_API_VER, &log_params);
        if (log_ == nullptr) {
            throw std::runtime_error("libplacebo log creation failed");
        }

        pl_vk_inst_params instance_params = pl_vk_inst_default_params;
        instance_params.extensions = extensions;
        instance_params.num_extensions = static_cast<int>(extension_count);
        instance_ = pl_vk_inst_create(log_, &instance_params);
        if (instance_ == nullptr) {
            throw std::runtime_error("Vulkan instance creation failed");
        }
        if (!SDL_Vulkan_CreateSurface(window_, instance_->instance, nullptr, &surface_)) {
            throw_sdl_error("SDL Vulkan surface creation failed");
        }

        pl_vulkan_params vulkan_params = pl_vulkan_default_params;
        vulkan_params.instance = instance_->instance;
        vulkan_params.get_proc_addr = instance_->get_proc_addr;
        vulkan_params.surface = surface_;
        vulkan_params.async_transfer = false;
        vulkan_params.async_compute = false;
        vulkan_params.queue_count = 1;
        vulkan_ = pl_vulkan_create(log_, &vulkan_params);
        if (vulkan_ == nullptr) {
            throw std::runtime_error("Vulkan device creation failed");
        }

        const pl_vulkan_swapchain_params swapchain_params{
            .surface = surface_,
            .present_mode = VK_PRESENT_MODE_MAILBOX_KHR,
            .swapchain_depth = 1,
            .allow_suboptimal = true,
            .disable_10bit_sdr = true,
            .alpha_bits = 0U,
            .color_bits = 0U,
        };
        swapchain_ = pl_vulkan_create_swapchain(vulkan_, &swapchain_params);
        if (swapchain_ == nullptr) {
            throw std::runtime_error("Vulkan swapchain creation failed");
        }
        renderer_ = pl_renderer_create(log_, vulkan_->gpu);
        if (renderer_ == nullptr) {
            throw std::runtime_error("libplacebo renderer creation failed");
        }
    } catch (...) {
        destroy();
        throw;
    }
}

VulkanPresenter::~VulkanPresenter()
{
    destroy();
}

void VulkanPresenter::destroy()
{
    pl_renderer_destroy(&renderer_);
    pl_swapchain_destroy(&swapchain_);
    pl_vulkan_destroy(&vulkan_);
    if (surface_ != VK_NULL_HANDLE && instance_ != nullptr) {
        SDL_Vulkan_DestroySurface(instance_->instance, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    pl_vk_inst_destroy(&instance_);
    pl_log_destroy(&log_);
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

void VulkanPresenter::poll_events()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            close_requested_ = true;
        }
    }
}

void VulkanPresenter::present(const AVFrame& frame)
{
    poll_events();
    if (close_requested_) {
        return;
    }

    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) {
        throw_sdl_error("SDL pixel-size query failed");
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    if (!pl_swapchain_resize(swapchain_, &width, &height)) {
        throw std::runtime_error("Vulkan swapchain resize failed");
    }

    pl_frame image{};
    const pl_avframe_params mapping_params{
        .frame = &frame,
        .tex = nullptr,
        .map_dovi = false,
    };
    if (!pl_map_avframe_ex(vulkan_->gpu, &image, &mapping_params)) {
        throw std::runtime_error("could not import VAAPI frame into Vulkan");
    }

    pl_swapchain_frame swapchain_frame{};
    const bool started = pl_swapchain_start_frame(swapchain_, &swapchain_frame);
    if (!started) {
        pl_unmap_avframe(vulkan_->gpu, &image);
        return;
    }
    pl_frame target{};
    pl_frame_from_swapchain(&target, &swapchain_frame);
    const bool rendered = pl_render_image(renderer_, &image, &target, &pl_render_fast_params);
    const bool submitted = pl_swapchain_submit_frame(swapchain_);
    if (rendered && submitted) {
        pl_swapchain_swap_buffers(swapchain_);
        ++presented_frames_;
    }
    pl_unmap_avframe(vulkan_->gpu, &image);
    if (!rendered || !submitted) {
        throw std::runtime_error("Vulkan frame render or submit failed");
    }
}

bool VulkanPresenter::close_requested() const
{
    return close_requested_;
}

std::uint64_t VulkanPresenter::presented_frames() const
{
    return presented_frames_;
}

} // namespace teleop
