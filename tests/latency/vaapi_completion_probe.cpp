// Diagnostic only: production H.264 decoder with explicit surface completion.
#include "h264_decoder.hpp"
#include <teleop/performance.hpp>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}
#include <va/va.h>
#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct Sample {
    std::int64_t pts{};
    std::uint64_t available{}, sync_begin{}, completed{};
    VASurfaceStatus before{};
};

struct Input {
    std::vector<std::byte> bytes;
    std::vector<std::pair<std::size_t, std::size_t>> offsets;
};

Input read_input(const char* file, const char* offsets)
{
    std::ifstream stream{file, std::ios::binary | std::ios::ate};
    if (!stream || stream.tellg() <= 0) throw std::runtime_error("missing/empty H.264 input");
    Input input;
    input.bytes.resize(static_cast<std::size_t>(stream.tellg()));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(input.bytes.data()), static_cast<std::streamsize>(input.bytes.size()));
    if (!stream) throw std::runtime_error("H.264 read failed");
    std::ifstream index{offsets};
    std::size_t pos{}, size{};
    while (index >> pos >> size) {
        if (size == 0 || pos > input.bytes.size() || size > input.bytes.size() - pos ||
            size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("invalid AU bounds");
        input.offsets.emplace_back(pos, size);
    }
    if (!index.eof() || input.offsets.empty() || input.offsets.size() > 300U)
        throw std::runtime_error("offsets must contain 1..300 AUs");
    return input;
}

class CompletionSink final : public teleop::DecodedFrameSink {
public:
    explicit CompletionSink(bool synchronize) : synchronize_(synchronize) { samples.reserve(300); }
    void present(const AVFrame& frame) override
    {
        Sample sample{.pts = frame.pts, .available = teleop::performance_time_ns()};
        if (frame.format != AV_PIX_FMT_VAAPI || !frame.hw_frames_ctx || frame.pts <= 0)
            throw std::runtime_error("missing VAAPI frame/timestamp association");
        const auto* frames = reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data);
        const auto* device = reinterpret_cast<const AVVAAPIDeviceContext*>(frames->device_ctx->hwctx);
        const auto surface = static_cast<VASurfaceID>(reinterpret_cast<std::uintptr_t>(frame.data[3]));
        const auto query = vaQuerySurfaceStatus(device->display, surface, &sample.before);
        if (query != VA_STATUS_SUCCESS) throw std::runtime_error(vaErrorStr(query));
        sample.sync_begin = teleop::performance_time_ns();
        if (synchronize_) {
            const auto result = vaSyncSurface2(device->display, surface, 200'000'000U);
            if (result != VA_STATUS_SUCCESS) throw std::runtime_error(vaErrorStr(result));
        }
        sample.completed = teleop::performance_time_ns();
        samples.push_back(sample);
    }
    std::string_view timing_stage() const override { return "probe_surface_sync"; }
    std::vector<Sample> samples;
private:
    bool synchronize_{};
};

int main(int argc, char** argv)
{
    try {
        if (argc != 5 && argc != 7)
            throw std::runtime_error("usage: probe sync|async PERIOD_US H264 OFFSETS [H264 OFFSETS]");
        if (!teleop::software_metrics_enabled()) throw std::runtime_error("set TELEOP_METRICS=1");
        const std::string mode{argv[1]};
        if (mode != "sync" && mode != "async") throw std::runtime_error("invalid mode");
        const auto period = std::chrono::microseconds{std::stoll(argv[2])};
        if (period.count() < 0 || period.count() > 100'000) throw std::runtime_error("invalid period");
        std::vector<Input> inputs;
        for (int i = 3; i < argc; i += 2) inputs.push_back(read_input(argv[i], argv[i + 1]));
        std::vector<std::vector<Sample>> samples(inputs.size());
        std::vector<std::exception_ptr> errors(inputs.size());
        std::barrier start{static_cast<std::ptrdiff_t>(inputs.size())};
        std::vector<std::jthread> workers;
        for (std::size_t stream = 0; stream < inputs.size(); ++stream) {
            workers.emplace_back([&, stream] {
                bool started = false;
                try {
                    CompletionSink sink{mode == "sync"};
                    teleop::H264VaapiDecoder decoder{"/dev/dri/renderD128", &sink, true,
                                                     static_cast<std::uint16_t>(stream)};
                    start.arrive_and_wait();
                    started = true;
                    const auto epoch = std::chrono::steady_clock::now();
                    const auto& input = inputs[stream];
                    std::vector<std::uint64_t> pts;
                    for (std::size_t i = 0; i < input.offsets.size(); ++i) {
                        std::this_thread::sleep_until(epoch + period * i);
                        const auto ready = teleop::performance_time_ns();
                        pts.push_back(ready);
                        const auto [pos, size] = input.offsets[i];
                        decoder.submit_access_unit(std::span<const std::byte>{input.bytes}.subspan(pos, size), ready);
                    }
                    decoder.flush();
                    if (decoder.statistics().hardware_frames != pts.size() || sink.samples.size() != pts.size())
                        throw std::runtime_error("hardware frame count mismatch");
                    for (std::size_t i = 0; i < pts.size(); ++i)
                        if (static_cast<std::uint64_t>(sink.samples[i].pts) != pts[i])
                            throw std::runtime_error("AU/frame association mismatch; B-frames are not supported");
                    samples[stream] = std::move(sink.samples);
                } catch (...) {
                    if (!started) start.arrive_and_drop();
                    errors[stream] = std::current_exception();
                }
            });
        }
        workers.clear(); // Join before reading samples/errors; also joins on exceptions.
        for (const auto& error : errors) if (error) std::rethrow_exception(error);
        std::puts("stream,frame,available_ms,query_ms,sync_wait_ms,ready_to_completion_ms,status_before");
        for (std::size_t stream = 0; stream < samples.size(); ++stream) {
            for (std::size_t i = 0; i < samples[stream].size(); ++i) {
                const auto& s = samples[stream][i];
                const auto ready = static_cast<std::uint64_t>(s.pts);
                std::printf("%zu,%zu,%.6f,%.6f,%.6f,%.6f,%d\n", stream, i,
                    static_cast<double>(s.available - ready) / 1e6,
                    static_cast<double>(s.sync_begin - s.available) / 1e6,
                    static_cast<double>(s.completed - s.sync_begin) / 1e6,
                    static_cast<double>(s.completed - ready) / 1e6,
                    static_cast<int>(s.before));
            }
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "vaapi-completion-probe: %s\n", error.what());
        return 1;
    }
}
