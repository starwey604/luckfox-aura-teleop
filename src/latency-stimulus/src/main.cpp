#include <SDL3/SDL.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

#include <time.h>

namespace {

struct Options {
    int width{1080};
    int height{900};
    int x{0};
    int y{0};
    bool position_specified{false};
    bool borderless{false};
    bool vsync{true};
    std::uint64_t max_frames{0U};
};

struct Color {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

constexpr Color kBackground{5U, 9U, 15U};
constexpr Color kForeground{242U, 247U, 255U};
constexpr Color kInactiveSegment{27U, 39U, 54U};
constexpr Color kCyan{0U, 224U, 255U};
constexpr Color kMagenta{255U, 41U, 180U};

// Bits are top, upper-right, lower-right, bottom, lower-left, upper-left, middle.
constexpr std::array<std::uint8_t, 10U> kDigitSegments{
    0x3fU, 0x06U, 0x5bU, 0x4fU, 0x66U,
    0x6dU, 0x7dU, 0x07U, 0x7fU, 0x6fU,
};

[[noreturn]] void throw_sdl_error(const char* operation)
{
    throw std::runtime_error(std::string{operation} + ": " + SDL_GetError());
}

void require_sdl(bool result, const char* operation)
{
    if (!result) {
        throw_sdl_error(operation);
    }
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer& output)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

void print_usage(const char* program)
{
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "Display an optical time code for Aura screen-to-screen latency tests.\n"
        "Run this on the CachyOS host, not inside Distrobox. The Aura camera\n"
        "should capture this window; place host-viewer below it on the rotated\n"
        "portrait 144 Hz auxiliary display.\n"
        "\n"
        "  --width PIXELS       Client width (default: 1080)\n"
        "  --height PIXELS      Client height (default: 900)\n"
        "  --x PIXELS           Window left position; requires --y\n"
        "  --y PIXELS           Window top position; requires --x\n"
        "  --borderless         Remove window decorations\n"
        "  --vsync on|off       Require or disable renderer VSync (default: on)\n"
        "  --frames COUNT       Exit after COUNT presents; 0 runs until closed\n",
        program);
}

bool parse_options(int argc, char* argv[], Options& options)
{
    bool has_x = false;
    bool has_y = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--help") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (option == "--borderless") {
            options.borderless = true;
            continue;
        }
        if (index + 1 >= argc) {
            return false;
        }
        const std::string_view value{argv[++index]};
        if (option == "--width") {
            if (!parse_integer(value, options.width) || options.width <= 0) {
                return false;
            }
        } else if (option == "--height") {
            if (!parse_integer(value, options.height) || options.height <= 0) {
                return false;
            }
        } else if (option == "--x") {
            if (!parse_integer(value, options.x)) {
                return false;
            }
            has_x = true;
        } else if (option == "--y") {
            if (!parse_integer(value, options.y)) {
                return false;
            }
            has_y = true;
        } else if (option == "--frames") {
            if (!parse_integer(value, options.max_frames)) {
                return false;
            }
        } else if (option == "--vsync") {
            if (value == "on") {
                options.vsync = true;
            } else if (value == "off") {
                options.vsync = false;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    options.position_specified = has_x && has_y;
    return has_x == has_y;
}

[[nodiscard]] std::uint64_t monotonic_raw_ns()
{
    timespec timestamp{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &timestamp) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC_RAW) failed");
    }
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(timestamp.tv_nsec);
}

void set_color(SDL_Renderer* renderer, Color color)
{
    require_sdl(SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, SDL_ALPHA_OPAQUE),
                "SDL draw-color setup failed");
}

void fill_rect(SDL_Renderer* renderer, float x, float y, float width, float height, Color color)
{
    set_color(renderer, color);
    const SDL_FRect rectangle{.x = x, .y = y, .w = width, .h = height};
    require_sdl(SDL_RenderFillRect(renderer, &rectangle), "SDL rectangle draw failed");
}

void draw_digit(SDL_Renderer* renderer, int digit, float x, float y, float width, float height,
                Color on_color)
{
    const std::uint8_t active = digit >= 0 && digit <= 9 ? kDigitSegments[static_cast<std::size_t>(digit)] : 0U;
    const float thickness = width * 0.16F;
    const float half_height = height * 0.5F;
    const std::array<SDL_FRect, 7U> rectangles{{
        {.x = x + thickness, .y = y, .w = width - 2.0F * thickness, .h = thickness},
        {.x = x + width - thickness, .y = y + thickness, .w = thickness, .h = half_height - thickness},
        {.x = x + width - thickness, .y = y + half_height, .w = thickness, .h = half_height - thickness},
        {.x = x + thickness, .y = y + height - thickness, .w = width - 2.0F * thickness, .h = thickness},
        {.x = x, .y = y + half_height, .w = thickness, .h = half_height - thickness},
        {.x = x, .y = y + thickness, .w = thickness, .h = half_height - thickness},
        {.x = x + thickness, .y = y + half_height - thickness * 0.5F,
         .w = width - 2.0F * thickness, .h = thickness},
    }};
    for (std::size_t index = 0U; index < rectangles.size(); ++index) {
        const bool enabled = (active & (1U << index)) != 0U;
        set_color(renderer, enabled ? on_color : kInactiveSegment);
        require_sdl(SDL_RenderFillRect(renderer, &rectangles[index]), "SDL seven-segment draw failed");
    }
}

void draw_colon(SDL_Renderer* renderer, float x, float y, float width, float height, Color color)
{
    const float side = width * 0.42F;
    fill_rect(renderer, x, y + height * 0.29F, side, side, color);
    fill_rect(renderer, x, y + height * 0.70F, side, side, color);
}

void draw_dot(SDL_Renderer* renderer, float x, float y, float width, float height, Color color)
{
    const float side = width * 0.42F;
    fill_rect(renderer, x, y + height - side, side, side, color);
}

void draw_timecode(SDL_Renderer* renderer, std::uint64_t milliseconds, int width, int height)
{
    const std::uint64_t total_seconds = milliseconds / 1000ULL;
    const std::uint64_t minutes = (total_seconds / 60ULL) % 100ULL;
    const std::uint64_t seconds = total_seconds % 60ULL;
    const std::uint64_t millis = milliseconds % 1000ULL;
    const std::array<int, 7U> digits{{
        static_cast<int>(minutes / 10ULL), static_cast<int>(minutes % 10ULL),
        static_cast<int>(seconds / 10ULL), static_cast<int>(seconds % 10ULL),
        static_cast<int>(millis / 100ULL), static_cast<int>((millis / 10ULL) % 10ULL),
        static_cast<int>(millis % 10ULL),
    }};
    const float digit_width = static_cast<float>(width) * 0.095F;
    const float digit_height = static_cast<float>(height) * 0.42F;
    const float spacing = digit_width * 0.14F;
    const float punctuation_width = digit_width * 0.42F;
    const float total_width = digit_width * 7.0F + spacing * 8.0F + punctuation_width * 2.0F;
    float x = (static_cast<float>(width) - total_width) * 0.5F;
    const float y = static_cast<float>(height) * 0.23F;
    for (std::size_t index = 0U; index < digits.size(); ++index) {
        draw_digit(renderer, digits[index], x, y, digit_width, digit_height, kForeground);
        x += digit_width + spacing;
        if (index == 1U) {
            draw_colon(renderer, x, y, punctuation_width, digit_height, kCyan);
            x += punctuation_width + spacing;
        }
        if (index == 3U) {
            draw_dot(renderer, x, y, punctuation_width, digit_height, kCyan);
            x += punctuation_width + spacing;
        }
    }
}

void draw_gray_code(SDL_Renderer* renderer, std::uint64_t milliseconds, int width, int height)
{
    const std::uint16_t gray = static_cast<std::uint16_t>(milliseconds ^ (milliseconds >> 1U));
    const float gap = static_cast<float>(width) * 0.006F;
    const float tile = (static_cast<float>(width) * 0.90F - gap * 15.0F) / 16.0F;
    const float start_x = (static_cast<float>(width) - (tile * 16.0F + gap * 15.0F)) * 0.5F;
    const float y = static_cast<float>(height) * 0.77F;
    for (unsigned int bit = 0U; bit < 16U; ++bit) {
        const bool one = (gray & (1U << (15U - bit))) != 0U;
        const float x = start_x + static_cast<float>(bit) * (tile + gap);
        fill_rect(renderer, x, y, tile, tile, one ? kForeground : kInactiveSegment);
    }
}

void draw_frame_counter(SDL_Renderer* renderer, std::uint64_t frame_index, int width, int height)
{
    std::array<int, 6U> digits{};
    std::uint64_t value = frame_index % 1000000ULL;
    for (std::size_t offset = 0U; offset < digits.size(); ++offset) {
        const std::size_t index = digits.size() - 1U - offset;
        digits[index] = static_cast<int>(value % 10ULL);
        value /= 10ULL;
    }
    const float digit_width = static_cast<float>(width) * 0.040F;
    const float digit_height = static_cast<float>(height) * 0.115F;
    const float spacing = digit_width * 0.18F;
    const float total_width = digit_width * 6.0F + spacing * 5.0F;
    float x = (static_cast<float>(width) - total_width) * 0.5F;
    const float y = static_cast<float>(height) * 0.075F;
    for (const int digit : digits) {
        draw_digit(renderer, digit, x, y, digit_width, digit_height, kCyan);
        x += digit_width + spacing;
    }
}

void draw_corner_markers(SDL_Renderer* renderer, int width, int height, Color color)
{
    const float side = static_cast<float>(width < height ? width : height) * 0.055F;
    const float margin = side * 0.50F;
    const std::array<SDL_FRect, 4U> rectangles{{
        {.x = margin, .y = margin, .w = side, .h = side},
        {.x = static_cast<float>(width) - margin - side, .y = margin, .w = side, .h = side},
        {.x = margin, .y = static_cast<float>(height) - margin - side, .w = side, .h = side},
        {.x = static_cast<float>(width) - margin - side, .y = static_cast<float>(height) - margin - side,
         .w = side, .h = side},
    }};
    set_color(renderer, color);
    require_sdl(SDL_RenderFillRects(renderer, rectangles.data(), static_cast<int>(rectangles.size())),
                "SDL corner-marker draw failed");
}

void draw_frame(SDL_Renderer* renderer, std::uint64_t timestamp_ns, std::uint64_t frame_index,
                int width, int height)
{
    const Color border = ((frame_index / 100ULL) % 2ULL) == 0ULL ? kCyan : kMagenta;
    set_color(renderer, kBackground);
    require_sdl(SDL_RenderClear(renderer), "SDL clear failed");

    const float border_width = static_cast<float>(width < height ? width : height) * 0.012F;
    fill_rect(renderer, 0.0F, 0.0F, static_cast<float>(width), border_width, border);
    fill_rect(renderer, 0.0F, static_cast<float>(height) - border_width, static_cast<float>(width), border_width,
              border);
    fill_rect(renderer, 0.0F, 0.0F, border_width, static_cast<float>(height), border);
    fill_rect(renderer, static_cast<float>(width) - border_width, 0.0F, border_width,
              static_cast<float>(height), border);
    draw_corner_markers(renderer, width, height, border);
    draw_frame_counter(renderer, frame_index, width, height);
    draw_timecode(renderer, timestamp_ns / 1000000ULL, width, height);
    draw_gray_code(renderer, timestamp_ns / 1000000ULL, width, height);
}

void poll_events(bool& close_requested)
{
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            close_requested = true;
        }
    }
}

int run(const Options& options)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw_sdl_error("SDL video initialization failed");
    }
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    try {
        const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE |
                                      (options.borderless ? SDL_WINDOW_BORDERLESS : 0U);
        window = SDL_CreateWindow("Aura optical latency stimulus", options.width, options.height, flags);
        if (window == nullptr) {
            throw_sdl_error("SDL window creation failed");
        }
        bool position_applied = !options.position_specified;
        if (options.position_specified) {
            position_applied = SDL_SetWindowPosition(window, options.x, options.y);
            if (!position_applied) {
                std::fprintf(stderr,
                             "latency-stimulus: compositor did not accept --x/--y (%s); "
                             "place this window in the upper half with KWin\n",
                             SDL_GetError());
            }
        }
        renderer = SDL_CreateRenderer(window, "vulkan");
        if (renderer == nullptr) {
            throw_sdl_error("SDL Vulkan renderer creation failed");
        }
        require_sdl(SDL_SetRenderVSync(renderer, options.vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED),
                    "SDL VSync configuration failed");

        int reported_vsync = SDL_RENDERER_VSYNC_DISABLED;
        require_sdl(SDL_GetRenderVSync(renderer, &reported_vsync), "SDL VSync query failed");
        std::printf("latency-stimulus: renderer=%s vsync=%d client=%dx%d position=%s\n",
                    SDL_GetRendererName(renderer), reported_vsync, options.width, options.height,
                    position_applied ? "explicit-or-default" : "window-manager");

        bool close_requested = false;
        std::uint64_t frame_index = 0U;
        std::uint64_t report_start_ns = monotonic_raw_ns();
        std::uint64_t report_frames = 0U;
        while (!close_requested && (options.max_frames == 0U || frame_index < options.max_frames)) {
            poll_events(close_requested);
            if (close_requested) {
                break;
            }
            int output_width = 0;
            int output_height = 0;
            require_sdl(SDL_GetRenderOutputSize(renderer, &output_width, &output_height),
                        "SDL output-size query failed");
            if (output_width > 0 && output_height > 0) {
                const std::uint64_t timestamp_ns = monotonic_raw_ns();
                draw_frame(renderer, timestamp_ns, frame_index, output_width, output_height);
                require_sdl(SDL_RenderPresent(renderer), "SDL present failed");
                ++frame_index;
                ++report_frames;
            }
            const std::uint64_t now_ns = monotonic_raw_ns();
            const std::uint64_t elapsed_ns = now_ns - report_start_ns;
            if (elapsed_ns >= 1000000000ULL) {
                const double fps = static_cast<double>(report_frames) * 1000000000.0 /
                                   static_cast<double>(elapsed_ns);
                std::printf("latency-stimulus: presents=%llu interval-fps=%.2f\n",
                            static_cast<unsigned long long>(frame_index), fps);
                report_start_ns = now_ns;
                report_frames = 0U;
            }
        }
        std::printf("latency-stimulus: stopped after %llu presents\n",
                    static_cast<unsigned long long>(frame_index));
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_SUCCESS;
    } catch (...) {
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        throw;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    try {
        return run(options);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "latency-stimulus: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
