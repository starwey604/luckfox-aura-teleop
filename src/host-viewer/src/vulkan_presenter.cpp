#include "vulkan_presenter.hpp"

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace teleop {
namespace {

[[noreturn]] void throw_sdl_error(const char* operation)
{
    throw std::runtime_error(std::string{operation} + ": " + SDL_GetError());
}

} // namespace

VulkanPresenter::VulkanPresenter(const VulkanWindowOptions& options)
    : dual_stream_(options.dual_stream)
{
    try {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw_sdl_error("SDL video initialization failed");
        }
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            throw_sdl_error("SDL Vulkan loader initialization failed");
        }
        const SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                      (options.borderless ? SDL_WINDOW_BORDERLESS : 0U);
        window_ = SDL_CreateWindow("Luckfox Aura Teleop", options.width, options.height, flags);
        if (window_ == nullptr) {
            throw_sdl_error("SDL window creation failed");
        }
        if (options.position_specified && !SDL_SetWindowPosition(window_, options.x, options.y)) {
            std::fprintf(stderr,
                         "host-viewer: compositor did not accept --window-x/--window-y (%s); "
                         "place this window in the lower half with KWin\n",
                         SDL_GetError());
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
    for (AVFrame*& frame : latest_frames_) {
        av_frame_free(&frame);
    }
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
    present_stream(0U, frame);
}

void VulkanPresenter::present_stream(std::uint16_t stream_id, const AVFrame& frame)
{
    const std::size_t stream_count = dual_stream_ ? 2U : 1U;
    if (stream_id >= stream_count) {
        throw std::runtime_error("received a video frame for an unsupported stream");
    }
    AVFrame* retained = av_frame_clone(&frame);
    if (retained == nullptr) {
        throw std::runtime_error("could not retain decoded video frame");
    }
    av_frame_free(&latest_frames_[stream_id]);
    latest_frames_[stream_id] = retained;
    render_latest_frames();
}

void VulkanPresenter::render_latest_frames()
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

    const std::size_t stream_count = dual_stream_ ? 2U : 1U;
    pl_swapchain_frame swapchain_frame{};
    const bool started = pl_swapchain_start_frame(swapchain_, &swapchain_frame);
    if (!started) {
        return;
    }
    pl_frame target{};
    pl_frame_from_swapchain(&target, &swapchain_frame);
    const float black[3]{0.0F, 0.0F, 0.0F};
    pl_frame_clear(vulkan_->gpu, &target, black);
    pl_render_params render_params = pl_render_fast_params;
    // In stereo mode each call owns only one target half. The default border
    // clear would erase the other half after it has been rendered.
    render_params.border = PL_CLEAR_SKIP;
    bool rendered = false;
    for (std::size_t stream = 0U; stream < stream_count; ++stream) {
        if (latest_frames_[stream] == nullptr) continue;
        pl_frame image{};
        const pl_avframe_params mapping_params{
            .frame = latest_frames_[stream],
            .tex = nullptr,
            .map_dovi = false,
        };
        if (!pl_map_avframe_ex(vulkan_->gpu, &image, &mapping_params)) {
            throw std::runtime_error("could not import decoded frame into Vulkan");
        }
        pl_frame target_region = target;
        if (dual_stream_) {
            const int y0 = static_cast<int>(stream) * height / 2;
            const int y1 = (stream == 0U) ? height / 2 : height;
            target_region.crop = {.x0 = 0.0F,
                                  .y0 = static_cast<float>(y0),
                                  .x1 = static_cast<float>(width),
                                  .y1 = static_cast<float>(y1)};
        }
        const bool stream_rendered =
            pl_render_image(renderer_, &image, &target_region, &render_params);
        pl_unmap_avframe(vulkan_->gpu, &image);
        rendered = stream_rendered || rendered;
    }
    const bool submitted = pl_swapchain_submit_frame(swapchain_);
    if (rendered && submitted) {
        pl_swapchain_swap_buffers(swapchain_);
        ++presented_frames_;
    }
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
