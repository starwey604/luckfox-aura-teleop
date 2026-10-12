#include <teleop/protocol.hpp>
#include <teleop/statistics.hpp>
#include <teleop/udp_socket.hpp>

extern "C" {
#include <rk_defines.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_venc.h>
#include <rk_mpi_vi.h>
}

#include <array>
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

volatile std::sig_atomic_t g_stop_requested = 0;

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
        "  --output-h264 FILE     Also save complete encoded access units as Annex-B H.264\n"
        "  --vi-device ID         VI device (default: 0)\n"
        "  --vi-pipe ID           VI pipe (default: 0)\n"
        "  --vi-channel ID        VI channel (default: 0)\n"
        "  --venc-channel ID      VENC channel (default: 0)\n",
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
    bool max_width_set = false;
    bool max_height_set = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--help") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
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
    g_stop_requested = 1;
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
    explicit MppPipeline(const Options& options) : options_(options)
    {
        check(RK_MPI_SYS_Init(), "RK_MPI_SYS_Init");
        system_initialized_ = true;
        setup_vi_device();
        setup_vi_channel();
        setup_venc();
        bind();
    }

    MppPipeline(const MppPipeline&) = delete;
    MppPipeline& operator=(const MppPipeline&) = delete;

    ~MppPipeline()
    {
        if (bound_) RK_MPI_SYS_UnBind(&source_channel_, &destination_channel_);
        if (venc_started_) RK_MPI_VENC_StopRecvFrame(options_.venc_channel);
        if (venc_created_) RK_MPI_VENC_DestroyChn(options_.venc_channel);
        if (vi_enabled_) RK_MPI_VI_DisableChn(options_.vi_pipe, options_.vi_channel);
        if (vi_device_enabled_by_us_) RK_MPI_VI_DisableDev(options_.vi_device);
        if (system_initialized_) RK_MPI_SYS_Exit();
    }

    // one RK_MPI_VENC_GetStream() call returns one frame, split into
    // u32PackCount packs (the low-delay slice split emits several).  The caller
    // must provide an array large enough for every pack of the frame.
    static constexpr std::size_t kMaximumPacks = 32U;

    bool get_stream(VENC_STREAM_S& stream, std::array<VENC_PACK_S, kMaximumPacks>& packs)
    {
        std::memset(&stream, 0, sizeof(stream));
        std::memset(packs.data(), 0, sizeof(VENC_PACK_S) * packs.size());
        stream.pstPack = packs.data();
        const RK_S32 result = RK_MPI_VENC_GetStream(options_.venc_channel, &stream, 1'000);
        if (result == RK_SUCCESS) {
            if (stream.u32PackCount > packs.size()) {
                std::fprintf(stderr,
                             "aura-sender: VENC returned %u packs for one frame, but only %zu "
                             "are supported\n",
                             stream.u32PackCount, packs.size());
                check(RK_MPI_VENC_ReleaseStream(options_.venc_channel, &stream),
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
                const RK_S32 status_result = RK_MPI_VENC_QueryStatus(options_.venc_channel, &status);
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
        check(RK_MPI_VENC_ReleaseStream(options_.venc_channel, &stream),
              "RK_MPI_VENC_ReleaseStream");
    }

private:
    void setup_vi_device()
    {
        VI_DEV_ATTR_S device_attributes{};
        const RK_S32 attributes_result = RK_MPI_VI_GetDevAttr(options_.vi_device, &device_attributes);
        if (attributes_result == RK_ERR_VI_NOT_CONFIG) {
            check(RK_MPI_VI_SetDevAttr(options_.vi_device, &device_attributes), "RK_MPI_VI_SetDevAttr");
        } else {
            check(attributes_result, "RK_MPI_VI_GetDevAttr");
        }

        if (RK_MPI_VI_GetDevIsEnable(options_.vi_device) != RK_SUCCESS) {
            check(RK_MPI_VI_EnableDev(options_.vi_device), "RK_MPI_VI_EnableDev");
            vi_device_enabled_by_us_ = true;
            VI_DEV_BIND_PIPE_S pipe_binding{};
            pipe_binding.u32Num = 1U;
            pipe_binding.PipeId[0] = options_.vi_pipe;
            check(RK_MPI_VI_SetDevBindPipe(options_.vi_device, &pipe_binding),
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
        check(RK_MPI_VI_SetChnAttr(options_.vi_pipe, options_.vi_channel, &attributes),
              "RK_MPI_VI_SetChnAttr");

        if (options_.wrap_lines != 0U) {
            VI_CHN_BUF_WRAP_S wrap{};
            wrap.bEnable = RK_TRUE;
            wrap.u32BufLine = options_.wrap_lines;
            wrap.u32WrapBufferSize = options_.wrap_lines * options_.width * 3U / 2U;
            check(RK_MPI_VI_SetChnWrapBufAttr(options_.vi_pipe, options_.vi_channel, &wrap),
                  "RK_MPI_VI_SetChnWrapBufAttr");
        }
        check(RK_MPI_VI_EnableChn(options_.vi_pipe, options_.vi_channel), "RK_MPI_VI_EnableChn");
        vi_enabled_ = true;
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
        check(RK_MPI_VENC_CreateChn(options_.venc_channel, &attributes), "RK_MPI_VENC_CreateChn");
        venc_created_ = true;

        if (options_.wrap_lines != 0U) {
            VENC_CHN_BUF_WRAP_S wrap{};
            wrap.bEnable = RK_TRUE;
            wrap.u32BufLine = options_.wrap_lines;
            wrap.u32WrapBufferSize = options_.wrap_lines * options_.width * 3U / 2U;
            check(RK_MPI_VENC_SetChnBufWrapAttr(options_.venc_channel, &wrap),
                  "RK_MPI_VENC_SetChnBufWrapAttr");
        }

        VENC_CHN_REF_BUF_SHARE_S reference_buffer{};
        reference_buffer.bEnable = RK_FALSE;
        check(RK_MPI_VENC_SetChnRefBufShareAttr(options_.venc_channel, &reference_buffer),
              "RK_MPI_VENC_SetChnRefBufShareAttr");

        VENC_SLICE_SPLIT_S slice_split{};
        check(RK_MPI_VENC_GetSliceSplit(options_.venc_channel, &slice_split),
              "RK_MPI_VENC_GetSliceSplit");
        slice_split.bSplitEnable = RK_TRUE;
        // Mode 3 is MB/CTU based and emits each slice as an individual packet.
        slice_split.u32SplitMode = 3U;
        slice_split.u32SplitSize = options_.slice_ctus;
        check(RK_MPI_VENC_SetSliceSplit(options_.venc_channel, &slice_split),
              "RK_MPI_VENC_SetSliceSplit");

        VENC_RECV_PIC_PARAM_S receive{};
        receive.s32RecvPicNum = -1;
        check(RK_MPI_VENC_StartRecvFrame(options_.venc_channel, &receive),
              "RK_MPI_VENC_StartRecvFrame");
        venc_started_ = true;
    }

    void bind()
    {
        source_channel_.enModId = RK_ID_VI;
        source_channel_.s32DevId = options_.vi_pipe;
        source_channel_.s32ChnId = options_.vi_channel;
        destination_channel_.enModId = RK_ID_VENC;
        destination_channel_.s32DevId = 0;
        destination_channel_.s32ChnId = options_.venc_channel;
        check(RK_MPI_SYS_Bind(&source_channel_, &destination_channel_), "RK_MPI_SYS_Bind(VI,VENC)");
        bound_ = true;
    }

    const Options& options_;
    MPP_CHN_S source_channel_{};
    MPP_CHN_S destination_channel_{};
    bool system_initialized_{false};
    bool vi_device_enabled_by_us_{false};
    bool vi_enabled_{false};
    bool venc_created_{false};
    bool venc_started_{false};
    bool bound_{false};
    std::uint32_t empty_stream_polls_{0U};
};

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
        teleop::UdpSender sender{options.destination, options.port};
        teleop::TransportStatistics statistics;
        MppPipeline pipeline{options};
        std::ofstream output;
        if (!options.output_h264.empty()) {
            output.open(std::string{options.output_h264}, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("could not open H.264 output file: " +
                                         std::string{options.output_h264});
            }
        }
        std::uint32_t encoded_frames = 0U;
        std::vector<std::byte> access_unit;
        std::uint64_t access_unit_pts_us = 0U;
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
            if (output.is_open()) {
                output.write(reinterpret_cast<const char*>(access_unit.data()),
                             static_cast<std::streamsize>(access_unit.size()));
                if (!output) {
                    throw std::runtime_error("could not write H.264 output file");
                }
            }
            if (!sender.send_frame(options.stream_id, encoded_frames, flags,
                                   pts_us_to_ns(access_unit_pts_us), access_unit, statistics)) {
                std::fprintf(stderr, "encoded frame %u was not completely queued for UDP transmission\n",
                             encoded_frames);
            }
            access_unit.clear();
            access_unit_is_key_frame = false;
            access_unit_open = false;
            ++encoded_frames;
        };

        std::array<VENC_PACK_S, MppPipeline::kMaximumPacks> packs{};
        const bool debug_packs = std::getenv("AURA_DEBUG_PACKS") != nullptr;

        while (g_stop_requested == 0 &&
               (options.frame_count == 0U || encoded_frames < options.frame_count)) {
            VENC_STREAM_S stream{};
            if (!pipeline.get_stream(stream, packs)) continue;

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
                        break;
                    }
                }
                const auto* base =
                    static_cast<const std::byte*>(RK_MPI_MB_Handle2VirAddr(pack.pMbBlk));
                if (base == nullptr) {
                    pipeline.release_stream(stream);
                    throw std::runtime_error("RK_MPI_MB_Handle2VirAddr returned null");
                }
                const auto* payload = base + pack.u32Offset;
                const std::span<const std::byte> packet{payload,
                                                        static_cast<std::size_t>(pack.u32Len)};
                if (!access_unit_open) {
                    access_unit_pts_us = pack.u64PTS;
                    access_unit_open = true;
                }
                if (packet.size() > teleop::kMaxEncodedAccessUnitSize - access_unit.size()) {
                    pipeline.release_stream(stream);
                    throw std::runtime_error("H.264 access unit exceeds the transport size limit");
                }
                access_unit.insert(access_unit.end(), packet.begin(), packet.end());
                access_unit_is_key_frame = access_unit_is_key_frame ||
                                           pack.DataType.enH264EType == H264E_NALU_IDRSLICE;
            }
            pipeline.release_stream(stream);
        }
        // The pending PTS has no following PTS to confirm that all slices
        // arrived. Drop it on shutdown instead of sending a truncated frame.

        std::printf("aura-sender: encoded-frames=%u datagrams=%llu bytes=%llu\n", encoded_frames,
                    static_cast<unsigned long long>(statistics.datagrams_sent),
                    static_cast<unsigned long long>(statistics.bytes_sent));
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "aura-sender: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
