#include "vulkan_presenter.hpp"
#include "receive_observer.hpp"
#include <teleop/performance.hpp>

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <charconv>
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
        const char* fail_acquire = std::getenv("TELEOP_TEST_FAIL_AFTER_SWAPCHAIN_ACQUIRE");
        test_fail_after_acquire_ = fail_acquire != nullptr && std::string_view{fail_acquire} == "1";
        // Test-only injection exercises SDL_QUIT with no incoming packets.
        if (const char* value = std::getenv("TELEOP_TEST_SDL_QUIT_AFTER_MS")) {
            const std::string text{value};
            std::uint32_t delay = 0U;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), delay);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || delay == 0U || delay > 60000U) {
                throw std::runtime_error("invalid TELEOP_TEST_SDL_QUIT_AFTER_MS");
            }
            test_quit_deadline_ns_ = viewer_monotonic_ns() + static_cast<std::uint64_t>(delay) * 1'000'000U;
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
        renderers_[0] = pl_renderer_create(log_, vulkan_->gpu);
        if (renderers_[0] == nullptr) {
            throw std::runtime_error("libplacebo renderer creation failed");
        }
        if (dual_stream_) {
            renderers_[1] = pl_renderer_create(log_, vulkan_->gpu);
            if (renderers_[1] == nullptr) {
                throw std::runtime_error("second libplacebo renderer creation failed");
            }
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
    if (vulkan_ != nullptr) {
        for (auto& textures : software_textures_) {
            for (auto& texture : textures) pl_tex_destroy(vulkan_->gpu, &texture);
        }
    }
    for (AVFrame*& frame : latest_frames_) {
        av_frame_free(&frame);
    }
    for (pl_renderer& renderer : renderers_) {
        pl_renderer_destroy(&renderer);
    }
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
    if (test_quit_deadline_ns_ != 0U && viewer_monotonic_ns() >= test_quit_deadline_ns_) {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        if (!SDL_PushEvent(&quit)) throw_sdl_error("SDL quit injection failed");
        test_quit_deadline_ns_ = 0U;
    }
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
    update_stream(stream_id, frame);
    render_latest_frames();
}

void VulkanPresenter::update_stream(std::uint16_t stream_id, const AVFrame& frame)
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
    const auto start_begin = performance_time_ns();
    const bool started = pl_swapchain_start_frame(swapchain_, &swapchain_frame);
    record_stage("host.display.start_frame", start_begin, performance_time_ns());
    if (!started) {
        return;
    }
    // An exception after acquiring a swapchain frame must still close it.
    // Otherwise pending GPU work can wait for a semaphore during destruction.
    struct AcquiredFrame {
        pl_swapchain swapchain;
        bool closed{};
        ~AcquiredFrame()
        {
            if (!closed && pl_swapchain_submit_frame(swapchain)) pl_swapchain_swap_buffers(swapchain);
        }
    } acquired{swapchain_};
    pl_frame target{};
    pl_frame_from_swapchain(&target, &swapchain_frame);
    const float black[3]{0.0F, 0.0F, 0.0F};
    pl_frame_clear(vulkan_->gpu, &target, black);
    if (test_fail_after_acquire_) throw std::runtime_error("injected failure after swapchain acquire");
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
            .tex = software_textures_[stream].data(),
            .map_dovi = false,
        };
        const std::string prefix = "host.stream" + std::to_string(stream) + ".";
        const auto map_begin = performance_time_ns();
        if (!pl_map_avframe_ex(vulkan_->gpu, &image, &mapping_params)) {
            throw std::runtime_error("could not import decoded frame into Vulkan");
        }
        record_stage(prefix + "map_call", map_begin, performance_time_ns());
        pl_frame target_region = target;
        if (dual_stream_) {
            const int y0 = static_cast<int>(stream) * height / 2;
            const int y1 = (stream == 0U) ? height / 2 : height;
            target_region.crop = {.x0 = 0.0F,
                                  .y0 = static_cast<float>(y0),
                                  .x1 = static_cast<float>(width),
                                  .y1 = static_cast<float>(y1)};
        }
        const auto render_begin = performance_time_ns();
        const bool stream_rendered =
            pl_render_image(renderers_[stream], &image, &target_region, &render_params);
        record_stage(prefix + "render_call", render_begin, performance_time_ns());
        const auto unmap_begin = performance_time_ns();
        pl_unmap_avframe(vulkan_->gpu, &image);
        record_stage(prefix + "unmap_call", unmap_begin, performance_time_ns());
        rendered = stream_rendered || rendered;
    }
    const auto submit_begin = performance_time_ns();
    const bool submitted = pl_swapchain_submit_frame(swapchain_);
    acquired.closed = true;
    record_stage("host.display.submit_frame", submit_begin, performance_time_ns());
    if (rendered && submitted) {
        const auto swap_begin = performance_time_ns();
        pl_swapchain_swap_buffers(swapchain_);
        record_stage("host.display.swap_buffers", swap_begin, performance_time_ns());
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
