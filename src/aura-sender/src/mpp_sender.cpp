#include <teleop/protocol.hpp>
#include <teleop/statistics.hpp>
#include <teleop/udp_socket.hpp>
#include <teleop/performance.hpp>

extern "C" {
#include <rk_defines.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_venc.h>
#include <rk_mpi_vi.h>
}

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <time.h>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::atomic<bool> g_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> g_cleanup_failed{false};
thread_local int g_camera = -1;

bool env_flag(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view{value} == "1";
}

std::uint64_t monotonic_ns()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

template <typename Call>
RK_S32 traced_call(const char* operation, Call&& call)
{
    const bool stream_call = std::string_view{operation}.starts_with("RK_MPI_VENC_GetStream(") ||
                             std::string_view{operation}.starts_with("RK_MPI_VENC_ReleaseStream(");
    const bool trace = env_flag("AURA_LIFECYCLE_TRACE") &&
                       (!stream_call || env_flag("AURA_TRACE_STREAM"));
    const auto begin = trace ? monotonic_ns() : 0U;
    if (trace) std::fprintf(stderr, "lifecycle: ns=%llu camera=%d BEGIN %s\n",
                           static_cast<unsigned long long>(begin), g_camera, operation);
    const RK_S32 result = call();
    if (trace) std::fprintf(stderr, "lifecycle: ns=%llu camera=%d END %s rc=%d elapsed_ms=%.3f\n",
                           static_cast<unsigned long long>(monotonic_ns()), g_camera, operation,
                           result, static_cast<double>(monotonic_ns() - begin) / 1'000'000.0);
    return result;
}
#define MPI_CALL(expression) traced_call(#expression, [&] { return (expression); })

void checkpoint(std::string_view phase)
{
    const char* requested = std::getenv("AURA_FAIL_AFTER");
    if (requested == nullptr) return;
    const std::string qualified = "camera" + std::to_string(g_camera) + "." + std::string{phase};
    if (phase == requested || qualified == requested) {
        throw std::runtime_error("injected failure after " + qualified);
    }
}

void cleanup_result(RK_S32 result, const char* operation) noexcept
{
    if (result == RK_SUCCESS) return;
    g_cleanup_failed.store(true);
    std::fprintf(stderr, "aura-sender: cleanup %s failed: %d\n", operation, result);
}

// The vendor MPI system is global. Reject a second sender process rather than
// letting one process exit/reconfigure resources owned by another process.
class MppSession {
public:
    MppSession()
    {
        lock_fd_ = open("/tmp/teleop-mpp.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (lock_fd_ < 0) throw std::runtime_error("could not open MPP process lock");
        if (flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            close(lock_fd_);
            lock_fd_ = -1;
            throw std::runtime_error("another MPI sender is running; use one sender --dual");
        }
        const RK_S32 result = MPI_CALL(RK_MPI_SYS_Init());
        if (result != RK_SUCCESS) {
            close(lock_fd_);
            lock_fd_ = -1;
            throw std::runtime_error("RK_MPI_SYS_Init failed: " + std::to_string(result));
        }
    }
    MppSession(const MppSession&) = delete;
    MppSession& operator=(const MppSession&) = delete;
    ~MppSession() { shutdown(); }
    void shutdown() noexcept
    {
        if (lock_fd_ < 0) return;
        g_camera = -1;
        cleanup_result(MPI_CALL(RK_MPI_SYS_WaitFreeMB()), "RK_MPI_SYS_WaitFreeMB");
        cleanup_result(MPI_CALL(RK_MPI_SYS_Exit()), "RK_MPI_SYS_Exit");
        close(lock_fd_);
        lock_fd_ = -1;
    }
private:
    int lock_fd_{-1};
};

struct Options {
    std::string_view destination{"192.168.1.2"};
    std::uint16_t port{5000U};
    std::uint16_t stream_id{0U};
    std::uint32_t frame_count{0U};
    std::uint32_t width{1280U};
    std::uint32_t height{1080U};
    std::uint32_t max_width{1280U};
    std::uint32_t max_height{1080U};
    std::uint32_t frame_rate{100U};
    std::uint32_t bitrate_kbps{30'000U};
    std::uint32_t gop{15U};
    std::uint32_t wrap_lines{256U};
    std::uint32_t slice_ctus{120U};
    std::string_view output_h264{};
    bool dual{false};
    int vi_device{0};
    int vi_pipe{0};
    int vi_channel{0};
    int venc_channel{0};
};

bool select_camera(Options& options, std::uint32_t camera)
{
    // Verified on the Aura stereo image: m00 is rkisp-vir0 and m01 is
    // rkisp-vir1. The two streams use matching VI pipe/device and VENC IDs.
    if (camera > 1U) {
        return false;
    }
    options.stream_id = static_cast<std::uint16_t>(camera);
    options.vi_device = static_cast<int>(camera);
    options.vi_pipe = static_cast<int>(camera);
    options.vi_channel = 0;
    options.venc_channel = static_cast<int>(camera);
    return true;
}

void print_usage(const char* program)
{
    std::printf(
        "Usage: %s [options]\n\n"
        "Capture NV12 from Aura VI, encode H.264 with RK MPI, and send every\n"
        "VENC packet by UDP. VI/VENC buffer wrap and one-slice-per-packet\n"
        "low-delay output are enabled by default.\n\n"
        "  --destination ADDRESS  UDP destination (default: 192.168.1.2)\n"
        "  --port PORT            UDP destination port (default: 5000)\n"
        "  --stream-id ID         Stream identifier (default: 0)\n"
        "  --dual                 Both cameras in one MPI session, one worker per stream\n"
        "  --camera INDEX         Aura stereo preset: 0=m00/rkisp-vir0, 1=m01/rkisp-vir1\n"
        "  --frames COUNT         Stop after COUNT encoded frames; 0 runs forever\n"
        "  --width PIXELS         VI/VENC output width (default: 1280)\n"
        "  --height PIXELS        VI/VENC output height (default: 1080)\n"
        "  --max-width PIXELS     Sensor maximum width (default: width)\n"
        "  --max-height PIXELS    Sensor maximum height (default: height)\n"
        "  --fps RATE             VI/VENC source and output frame rate (default: 100)\n"
        "  --bitrate-kbps RATE    H.264 CBR rate (default: 30000)\n"
        "  --gop FRAMES           IDR interval in frames (default: 15)\n"
        "  --wrap-lines LINES     VI/VENC wrap line count; 0 disables wrap (default: 256)\n"
        "  --slice-ctus COUNT     CTUs per slice, low-delay mode 3 (default: 120)\n"
        "  --output-h264 FILE     Save Annex-B; --dual requires {camera} in FILE\n"
        "  --vi-device ID         VI device (default: 0)\n"
        "  --vi-pipe ID           VI pipe (default: 0)\n"
        "  --vi-channel ID        VI channel (default: 0)\n"
        "  --venc-channel ID      VENC channel (default: 0)\n"
        "  AURA_LIFECYCLE_TRACE=1 Trace initialization/cleanup API begin/end and return codes\n"
        "  AURA_TRACE_STREAM=1    Also trace GetStream/ReleaseStream (debug only)\n"
        "  AURA_FAIL_AFTER=PHASE  Inject failure, e.g. system.initialized or camera1.venc.created (debug only)\n"
        "  TELEOP_METRICS=1       Opt-in local stage timings and socket buffer diagnostics\n",
        program);
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer& output)
{
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    if (value > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
        return false;
    }
    output = static_cast<Integer>(value);
    return true;
}

bool parse_options(int argc, char* argv[], Options& options)
{
    bool routing_set = false;
    bool max_width_set = false;
    bool max_height_set = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--help") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (option == "--dual") { options.dual = true; continue; }
        if (option == "--camera" || option == "--stream-id" || option.starts_with("--vi-") ||
            option == "--venc-channel") routing_set = true;
        if (index + 1 >= argc) {
            return false;
        }
        const std::string_view value{argv[++index]};
        if (option == "--destination") {
            options.destination = value;
        } else if (option == "--camera") {
            std::uint32_t camera = 0U;
            if (!parse_integer(value, camera) || !select_camera(options, camera)) return false;
        } else if (option == "--port") {
            if (!parse_integer(value, options.port) || options.port == 0U) return false;
        } else if (option == "--stream-id") {
            if (!parse_integer(value, options.stream_id)) return false;
        } else if (option == "--frames") {
            if (!parse_integer(value, options.frame_count)) return false;
        } else if (option == "--width") {
            if (!parse_integer(value, options.width) || options.width == 0U) return false;
        } else if (option == "--height") {
            if (!parse_integer(value, options.height) || options.height == 0U) return false;
        } else if (option == "--max-width") {
            if (!parse_integer(value, options.max_width) || options.max_width == 0U) return false;
            max_width_set = true;
        } else if (option == "--max-height") {
            if (!parse_integer(value, options.max_height) || options.max_height == 0U) return false;
            max_height_set = true;
        } else if (option == "--fps") {
            if (!parse_integer(value, options.frame_rate) || options.frame_rate == 0U) return false;
        } else if (option == "--bitrate-kbps") {
            if (!parse_integer(value, options.bitrate_kbps) || options.bitrate_kbps == 0U) return false;
        } else if (option == "--gop") {
            if (!parse_integer(value, options.gop) || options.gop == 0U) return false;
        } else if (option == "--wrap-lines") {
            if (!parse_integer(value, options.wrap_lines)) return false;
        } else if (option == "--slice-ctus") {
            if (!parse_integer(value, options.slice_ctus) || options.slice_ctus == 0U) return false;
        } else if (option == "--output-h264") {
            options.output_h264 = value;
        } else if (option == "--vi-device") {
            if (!parse_integer(value, options.vi_device)) return false;
        } else if (option == "--vi-pipe") {
            if (!parse_integer(value, options.vi_pipe)) return false;
        } else if (option == "--vi-channel") {
            if (!parse_integer(value, options.vi_channel)) return false;
        } else if (option == "--venc-channel") {
            if (!parse_integer(value, options.venc_channel)) return false;
        } else {
            return false;
        }
    }
    if (options.dual && (routing_set || (!options.output_h264.empty() &&
        options.output_h264.find("{camera}") == std::string_view::npos))) return false;
    if (!max_width_set) options.max_width = options.width;
    if (!max_height_set) options.max_height = options.height;
    return options.max_width >= options.width && options.max_height >= options.height &&
           (options.wrap_lines == 0U ||
            (options.wrap_lines >= 128U && options.wrap_lines <= options.height));
}

void check(RK_S32 result, const char* operation)
{
    if (result != RK_SUCCESS) {
        throw std::runtime_error(std::string{operation} + " failed: " + std::to_string(result));
    }
}

void handle_signal(int)
{
    g_stop_requested.store(true);
}

std::uint64_t pts_us_to_ns(RK_U64 pts_us)
{
    constexpr std::uint64_t kNsPerUs = 1'000U;
    if (pts_us > std::numeric_limits<std::uint64_t>::max() / kNsPerUs) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(pts_us) * kNsPerUs;
}

class MppPipeline {
public:
    explicit MppPipeline(const Options& options) : options_(options) {}

    // Store the object in its owner before acquiring resources. If any API
    // throws, main first joins all workers, then rolls back every partial pipe.
    void initialize()
    {
        g_camera = options_.vi_device;
        setup_vi_device();
        setup_vi_channel();
        setup_venc();
    }

    void activate()
    {
        g_camera = options_.vi_device;
        bind();
    }

    MppPipeline(const MppPipeline&) = delete;
    MppPipeline& operator=(const MppPipeline&) = delete;

    ~MppPipeline() { shutdown(); }

    // Stop all producers/bindings before destroying either stereo pipeline.
    void stop() noexcept
    {
        g_camera = options_.vi_device;
        if (bound_) {
            bound_ = false;
            cleanup_result(MPI_CALL(RK_MPI_SYS_UnBind(&source_channel_, &destination_channel_)), "SYS_UnBind");
        }
        if (venc_started_) {
            venc_started_ = false;
            cleanup_result(MPI_CALL(RK_MPI_VENC_StopRecvFrame(options_.venc_channel)), "VENC_StopRecvFrame");
        }
    }

    void shutdown() noexcept
    {
        stop();
        if (venc_created_) {
            venc_created_ = false;
            cleanup_result(MPI_CALL(RK_MPI_VENC_DestroyChn(options_.venc_channel)), "VENC_DestroyChn");
        }
        if (vi_enabled_) {
            vi_enabled_ = false;
            cleanup_result(MPI_CALL(RK_MPI_VI_DisableChn(options_.vi_pipe, options_.vi_channel)), "VI_DisableChn");
        }
        if (vi_device_enabled_by_us_) {
            vi_device_enabled_by_us_ = false;
            cleanup_result(MPI_CALL(RK_MPI_VI_DisableDev(options_.vi_device)), "VI_DisableDev");
        }
    }

    // The caller owns each returned stream until ReleaseStream. With slice
    // output, a stream may contain only part of a frame; assemble by PTS.
    static constexpr std::size_t kMaximumPacks = 32U;

    bool get_stream(VENC_STREAM_S& stream, std::array<VENC_PACK_S, kMaximumPacks>& packs)
    {
        std::memset(&stream, 0, sizeof(stream));
        std::memset(packs.data(), 0, sizeof(VENC_PACK_S) * packs.size());
        stream.pstPack = packs.data();
        const RK_S32 result = MPI_CALL(RK_MPI_VENC_GetStream(options_.venc_channel, &stream, 1'000));
        if (result == RK_SUCCESS) {
            if (stream.u32PackCount > packs.size()) {
                std::fprintf(stderr,
                             "aura-sender: VENC returned %u packs for one frame, but only %zu "
                             "are supported\n",
                             stream.u32PackCount, packs.size());
                check(MPI_CALL(RK_MPI_VENC_ReleaseStream(options_.venc_channel, &stream)),
                      "RK_MPI_VENC_ReleaseStream");
                return false;
            }
            return true;
        }
        // This Rockit SDK reports a normal GetStream wait expiry as its VENC
        // buffer-empty code, rather than the POSIX ETIMEDOUT used by some SDKs.
        if (result == -ETIMEDOUT || result == RK_ERR_VENC_BUF_EMPTY) {
            ++empty_stream_polls_;
            if (empty_stream_polls_ % 10U == 0U) {
                VENC_CHN_STATUS_S status{};
                const RK_S32 status_result = MPI_CALL(RK_MPI_VENC_QueryStatus(options_.venc_channel, &status));
                if (status_result == RK_SUCCESS) {
                    std::fprintf(stderr,
                                 "aura-sender: waiting for VENC stream: left-pics=%u "
                                 "left-stream-frames=%u left-stream-bytes=%u\n",
                                 status.u32LeftPics, status.u32LeftStreamFrames,
                                 status.u32LeftStreamBytes);
                }
            }
            return false;
        }
        check(result, "RK_MPI_VENC_GetStream");
        return false;
    }

    void release_stream(VENC_STREAM_S& stream)
    {
        check(MPI_CALL(RK_MPI_VENC_ReleaseStream(options_.venc_channel, &stream)),
              "RK_MPI_VENC_ReleaseStream");
    }

private:
    void setup_vi_device()
    {
        VI_DEV_ATTR_S device_attributes{};
        const RK_S32 attributes_result = MPI_CALL(RK_MPI_VI_GetDevAttr(options_.vi_device, &device_attributes));
        if (attributes_result == RK_ERR_VI_NOT_CONFIG) {
            check(MPI_CALL(RK_MPI_VI_SetDevAttr(options_.vi_device, &device_attributes)), "RK_MPI_VI_SetDevAttr");
        } else {
            check(attributes_result, "RK_MPI_VI_GetDevAttr");
        }

        if (MPI_CALL(RK_MPI_VI_GetDevIsEnable(options_.vi_device)) != RK_SUCCESS) {
            check(MPI_CALL(RK_MPI_VI_EnableDev(options_.vi_device)), "RK_MPI_VI_EnableDev");
            vi_device_enabled_by_us_ = true;
            checkpoint("vi.device");
            VI_DEV_BIND_PIPE_S pipe_binding{};
            pipe_binding.u32Num = 1U;
            pipe_binding.PipeId[0] = options_.vi_pipe;
            check(MPI_CALL(RK_MPI_VI_SetDevBindPipe(options_.vi_device, &pipe_binding)),
                  "RK_MPI_VI_SetDevBindPipe");
        }
    }

    void setup_vi_channel()
    {
        VI_CHN_ATTR_S attributes{};
        attributes.stIspOpt.u32BufCount = 3U;
        attributes.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
        attributes.stSize.u32Width = options_.width;
        attributes.stSize.u32Height = options_.height;
        attributes.enPixelFormat = RK_FMT_YUV420SP;
        attributes.stIspOpt.stMaxSize.u32Width = options_.max_width;
        attributes.stIspOpt.stMaxSize.u32Height = options_.max_height;
        attributes.u32Depth = 0U;
        // rkisp-vir0 on this Aura SDK rejects VVI_IOCTL_SET_FR. Leave the VI
        // rate at its auto (-1/-1) setting; the sensor mode supplies 100 fps
        // while VENC still receives options_.frame_rate for rate control.
        check(MPI_CALL(RK_MPI_VI_SetChnAttr(options_.vi_pipe, options_.vi_channel, &attributes)),
              "RK_MPI_VI_SetChnAttr");

        if (options_.wrap_lines != 0U) {
            VI_CHN_BUF_WRAP_S wrap{};
            wrap.bEnable = RK_TRUE;
            wrap.u32BufLine = options_.wrap_lines;
            wrap.u32WrapBufferSize = options_.wrap_lines * options_.width * 3U / 2U;
            check(MPI_CALL(RK_MPI_VI_SetChnWrapBufAttr(options_.vi_pipe, options_.vi_channel, &wrap)),
                  "RK_MPI_VI_SetChnWrapBufAttr");
        }
        check(MPI_CALL(RK_MPI_VI_EnableChn(options_.vi_pipe, options_.vi_channel)), "RK_MPI_VI_EnableChn");
        vi_enabled_ = true;
        checkpoint("vi.enabled");
    }

    void setup_venc()
    {
        VENC_CHN_ATTR_S attributes{};
        attributes.stVencAttr.enType = RK_VIDEO_ID_AVC;
        attributes.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
        attributes.stVencAttr.u32MaxPicWidth = options_.max_width;
        attributes.stVencAttr.u32MaxPicHeight = options_.max_height;
        attributes.stVencAttr.u32PicWidth = options_.width;
        attributes.stVencAttr.u32PicHeight = options_.height;
        attributes.stVencAttr.u32VirWidth = options_.width;
        attributes.stVencAttr.u32VirHeight = options_.height;
        attributes.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
        // The SDK VI→VENC wrap reference uses five stream buffers. Three is
        // legal but leaves too little headroom while slice packets are drained.
        attributes.stVencAttr.u32StreamBufCnt = 5U;
        attributes.stVencAttr.u32BufSize = options_.max_width * options_.max_height * 3U / 2U;
        attributes.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        attributes.stRcAttr.stH264Cbr.u32BitRate = options_.bitrate_kbps;
        attributes.stRcAttr.stH264Cbr.u32Gop = options_.gop;
        attributes.stRcAttr.stH264Cbr.u32SrcFrameRateNum = options_.frame_rate;
        attributes.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1U;
        attributes.stRcAttr.stH264Cbr.fr32DstFrameRateNum = options_.frame_rate;
        attributes.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1U;
        attributes.stRcAttr.stH264Cbr.u32StatTime = 1U;
        attributes.stGopAttr.u32MaxLtrCount = 0U;
        check(MPI_CALL(RK_MPI_VENC_CreateChn(options_.venc_channel, &attributes)), "RK_MPI_VENC_CreateChn");
        venc_created_ = true;
        checkpoint("venc.created");

        if (options_.wrap_lines != 0U) {
            VENC_CHN_BUF_WRAP_S wrap{};
            wrap.bEnable = RK_TRUE;
            wrap.u32BufLine = options_.wrap_lines;
            wrap.u32WrapBufferSize = options_.wrap_lines * options_.width * 3U / 2U;
            check(MPI_CALL(RK_MPI_VENC_SetChnBufWrapAttr(options_.venc_channel, &wrap)),
                  "RK_MPI_VENC_SetChnBufWrapAttr");
        }

        VENC_CHN_REF_BUF_SHARE_S reference_buffer{};
        reference_buffer.bEnable = RK_FALSE;
        check(MPI_CALL(RK_MPI_VENC_SetChnRefBufShareAttr(options_.venc_channel, &reference_buffer)),
              "RK_MPI_VENC_SetChnRefBufShareAttr");

        VENC_SLICE_SPLIT_S slice_split{};
        check(MPI_CALL(RK_MPI_VENC_GetSliceSplit(options_.venc_channel, &slice_split)),
              "RK_MPI_VENC_GetSliceSplit");
        slice_split.bSplitEnable = RK_TRUE;
        // Mode 3 is MB/CTU based and emits each slice as an individual packet.
        slice_split.u32SplitMode = 3U;
        slice_split.u32SplitSize = options_.slice_ctus;
        check(MPI_CALL(RK_MPI_VENC_SetSliceSplit(options_.venc_channel, &slice_split)),
              "RK_MPI_VENC_SetSliceSplit");

        VENC_RECV_PIC_PARAM_S receive{};
        receive.s32RecvPicNum = -1;
        check(MPI_CALL(RK_MPI_VENC_StartRecvFrame(options_.venc_channel, &receive)),
              "RK_MPI_VENC_StartRecvFrame");
        venc_started_ = true;
        checkpoint("venc.started");
    }

    void bind()
    {
        source_channel_.enModId = RK_ID_VI;
        source_channel_.s32DevId = options_.vi_pipe;
        source_channel_.s32ChnId = options_.vi_channel;
        destination_channel_.enModId = RK_ID_VENC;
        destination_channel_.s32DevId = 0;
        destination_channel_.s32ChnId = options_.venc_channel;
        check(MPI_CALL(RK_MPI_SYS_Bind(&source_channel_, &destination_channel_)), "RK_MPI_SYS_Bind(VI,VENC)");
        bound_ = true;
        checkpoint("bound");
    }

    const Options& options_;
    MPP_CHN_S source_channel_{};
    MPP_CHN_S destination_channel_{};
    bool vi_device_enabled_by_us_{false};
    bool vi_enabled_{false};
    bool venc_created_{false};
    bool venc_started_{false};
    bool bound_{false};
    std::uint32_t empty_stream_polls_{0U};
};

void capture_stream(const Options& options, MppPipeline& pipeline,
                    std::atomic<unsigned>& completed, unsigned worker_count)
{
    g_camera = options.vi_device;
    teleop::UdpSender sender{options.destination, options.port};
    teleop::TransportStatistics statistics;
    std::ofstream output;
    if (!options.output_h264.empty()) {
        std::string filename{options.output_h264};
        const auto marker = filename.find("{camera}");
        if (marker != std::string::npos) filename.replace(marker, 8U, std::to_string(options.stream_id));
        output.open(filename, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("could not open H.264 output file: " +
                                     std::string{options.output_h264});
        }
    }
    std::uint32_t encoded_frames = 0U;
    std::vector<std::byte> access_unit;
    std::uint64_t access_unit_pts_us = 0U;
    std::uint64_t first_pack_ns = 0U;
    std::uint64_t last_pack_ns = 0U;
    std::uint64_t previous_au_ns = 0U;
    bool access_unit_is_key_frame = false;
    bool access_unit_open = false;

    std::printf("aura-sender: VI %d/%d/%d -> H.264 VENC %d, %ux%u@%u, %u kbps, wrap=%u lines, "
                "slice low-delay=%u CTUs, UDP %.*s:%u\n",
                options.vi_device, options.vi_pipe, options.vi_channel, options.venc_channel,
                options.width, options.height, options.frame_rate, options.bitrate_kbps,
                options.wrap_lines, options.slice_ctus,
                static_cast<int>(options.destination.size()), options.destination.data(), options.port);
    std::fflush(stdout);

    // The low-delay slice split makes one RK_MPI_VENC_GetStream() call return
    // a single VENC_PACK_S and always set bFrameEnd, so the pack boundary is
    // not the frame boundary.  Every slice of a frame shares the same
    // u64PTS, so consecutive packs are grouped into one access unit by PTS.
    const auto send_access_unit = [&]() {
        if (access_unit.empty()) {
            access_unit_open = false;
            return;
        }
        const auto flags = access_unit_is_key_frame ? teleop::PacketFlags::kKeyFrame
                                                     : teleop::PacketFlags::kNone;
        const auto ready_ns = teleop::performance_time_ns();
        teleop::record_stage("sender.au_pack_span", first_pack_ns, last_pack_ns);
        teleop::record_stage("sender.next_pts_wait", last_pack_ns, ready_ns);
        teleop::record_stage("sender.first_pack_to_send", first_pack_ns, ready_ns);
        teleop::record_stage("sender.au_interval", previous_au_ns, ready_ns);
        previous_au_ns = ready_ns;
        if (output.is_open()) {
            output.write(reinterpret_cast<const char*>(access_unit.data()),
                         static_cast<std::streamsize>(access_unit.size()));
            if (!output) {
                throw std::runtime_error("could not write H.264 output file");
            }
        }
        const auto send_begin = teleop::performance_time_ns();
        if (!sender.send_frame(options.stream_id, encoded_frames, flags,
                               pts_us_to_ns(access_unit_pts_us), access_unit, statistics)) {
            std::fprintf(stderr, "encoded frame %u was not completely queued for UDP transmission\n",
                         encoded_frames);
        }
        teleop::record_stage("sender.udp_queue", send_begin, teleop::performance_time_ns());
        access_unit.clear();
        access_unit_is_key_frame = false;
        access_unit_open = false;
        ++encoded_frames;
    };

    std::array<VENC_PACK_S, MppPipeline::kMaximumPacks> packs{};
    const bool debug_packs = std::getenv("AURA_DEBUG_PACKS") != nullptr;

    bool finished = false;
    while (!g_stop_requested.load()) {
        VENC_STREAM_S stream{};
        const auto get_begin = teleop::performance_time_ns();
        if (!pipeline.get_stream(stream, packs)) continue;
        struct StreamLease {
            MppPipeline& pipeline;
            VENC_STREAM_S* stream;
            ~StreamLease() {
                if (!stream) return;
                try { pipeline.release_stream(*stream); }
                catch (const std::exception& error) {
                    g_cleanup_failed.store(true);
                    g_stop_requested.store(true);
                    std::fprintf(stderr, "aura-sender: %s\n", error.what());
                }
            }
            void release() {
                auto* owned = stream;
                stream = nullptr;
                pipeline.release_stream(*owned);
            }
        } lease{pipeline, &stream};
        checkpoint("stream.acquired");
        // Keep draining a completed stream until its sibling finishes,
        // avoiding backpressure on the shared ISP/encoder during teardown.
        if (finished) continue;
        const auto get_end = teleop::performance_time_ns();
        teleop::record_stage("sender.get_stream_wait", get_begin, get_end);

        if (debug_packs) {
            std::fprintf(stderr, "stream: packs=%u\n", stream.u32PackCount);
            for (RK_U32 index = 0U; index < stream.u32PackCount; ++index) {
                const VENC_PACK_S& dbg = packs[index];
                const auto* dbase = dbg.pMbBlk != nullptr
                    ? static_cast<const std::byte*>(RK_MPI_MB_Handle2VirAddr(dbg.pMbBlk))
                    : nullptr;
                std::fprintf(stderr, "  pack[%u] len=%u end=%d type=%d pts=%llu first=", index,
                             dbg.u32Len, dbg.bFrameEnd,
                             static_cast<int>(dbg.DataType.enH264EType),
                             static_cast<unsigned long long>(dbg.u64PTS));
                if (dbase != nullptr) {
                    for (RK_U32 byte = 0U; byte < 8U && byte < dbg.u32Len; ++byte) {
                        std::fprintf(stderr, "%02x",
                                     static_cast<unsigned>(dbase[dbg.u32Offset + byte]));
                    }
                }
                std::fprintf(stderr, "\n");
            }
        }

        for (RK_U32 index = 0U; index < stream.u32PackCount; ++index) {
            const VENC_PACK_S& pack = packs[index];
            if (pack.pMbBlk == nullptr || pack.u32Len == 0U) {
                continue;
            }
            // A change of PTS means the previous frame is complete.
            if (access_unit_open && pack.u64PTS != access_unit_pts_us) {
                send_access_unit();
                if (options.frame_count != 0U && encoded_frames >= options.frame_count) {
                    finished = true;
                    if (completed.fetch_add(1U) + 1U == worker_count) g_stop_requested.store(true);
                    break;
                }
            }
            const auto* base =
                static_cast<const std::byte*>(RK_MPI_MB_Handle2VirAddr(pack.pMbBlk));
            if (base == nullptr) {
                throw std::runtime_error("RK_MPI_MB_Handle2VirAddr returned null");
            }
            const auto* payload = base + pack.u32Offset;
            const std::span<const std::byte> packet{payload,
                                                    static_cast<std::size_t>(pack.u32Len)};
            if (!access_unit_open) {
                access_unit_pts_us = pack.u64PTS;
                access_unit_open = true;
                first_pack_ns = get_end;
            }
            last_pack_ns = get_end;
            if (packet.size() > teleop::kMaxEncodedAccessUnitSize - access_unit.size()) {
                throw std::runtime_error("H.264 access unit exceeds the transport size limit");
            }
            access_unit.insert(access_unit.end(), packet.begin(), packet.end());
            access_unit_is_key_frame = access_unit_is_key_frame ||
                                       pack.DataType.enH264EType == H264E_NALU_IDRSLICE;
        }
        lease.release();
    }
    // The pending PTS has no following PTS to confirm that all slices
    // arrived. Drop it on shutdown instead of sending a truncated frame.

    const std::string prefix = worker_count == 2U ? "stream" + std::to_string(options.stream_id) + "." : "";
    flockfile(stdout);
    std::printf("aura-sender: stream=%u encoded-frames=%u datagrams=%llu bytes=%llu\n", options.stream_id, encoded_frames,
                static_cast<unsigned long long>(statistics.datagrams_sent),
                static_cast<unsigned long long>(statistics.bytes_sent));
    teleop::print_software_metrics(prefix);
    std::fflush(stdout);
    funlockfile(stdout);
}

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    try {
        MppSession session;
        // Keep this outside the constructor so a diagnostic exception runs
        // the fully constructed session's normal SYS cleanup, without cameras.
        checkpoint("system.initialized");
        const unsigned worker_count = options.dual ? 2U : 1U;
        std::array<Options, 2> cameras{options, options};
        if (options.dual) {
            select_camera(cameras[0], 0U);
            select_camera(cameras[1], 1U);
        }
        std::array<std::unique_ptr<MppPipeline>, 2> pipelines;
        std::array<std::thread, 2> workers;
        std::array<std::exception_ptr, 2> errors{};
        std::atomic<unsigned> completed{0U};
        std::exception_ptr startup_error;
        try {
            // Match the SDK dual-camera example: configure ALL VI/VENC
            // resources before binding any VI producer to an active encoder.
            // Starting stream 0 while EnableChn(1) is pending can race changes
            // to the shared ISP with VENC GetStream/ReleaseStream.
            for (unsigned index = 0U; index < worker_count && !g_stop_requested.load(); ++index) {
                pipelines[index] = std::make_unique<MppPipeline>(cameras[index]);
                pipelines[index]->initialize();
            }
            for (unsigned index = 0U; index < worker_count && !g_stop_requested.load(); ++index) {
                pipelines[index]->activate();
            }
            for (unsigned index = 0U; index < worker_count && !g_stop_requested.load(); ++index) {
                workers[index] = std::thread([&, index] {
                    try { capture_stream(cameras[index], *pipelines[index], completed, worker_count); }
                    catch (...) {
                        errors[index] = std::current_exception();
                        g_stop_requested.store(true);
                    }
                });
            }
        } catch (...) {
            startup_error = std::current_exception();
            g_stop_requested.store(true);
        }
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        for (auto& pipeline : pipelines) if (pipeline) pipeline->stop();
        for (auto& pipeline : pipelines) if (pipeline) pipeline->shutdown();
        session.shutdown();
        if (startup_error) std::rethrow_exception(startup_error);
        for (const auto& error : errors) if (error) std::rethrow_exception(error);
        return g_cleanup_failed.load() ? EXIT_FAILURE : EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "aura-sender: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
