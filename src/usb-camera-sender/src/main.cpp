#include <teleop/protocol.hpp>
#include <teleop/statistics.hpp>
#include <teleop/udp_socket.hpp>

#include <linux/videodev2.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string_view device{"/dev/video0"};
    std::string_view destination{"127.0.0.1"};
    std::uint16_t port{5000U};
    std::uint16_t stream_id{0U};
    std::uint32_t width{1280U};
    std::uint32_t height{720U};
    std::uint32_t frames_per_second{60U};
    std::uint32_t frame_count{120U};
    std::uint32_t bitrate_kbps{8000U};
};

struct MappedBuffer {
    void* data{};
    std::size_t length{};
};

struct CapturedBuffer {
    std::uint32_t index{};
    std::span<const std::byte> data;
};

[[nodiscard]] std::string ffmpeg_error(int error_code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    av_strerror(error_code, message.data(), message.size());
    return {message.data()};
}

[[nodiscard]] std::uint64_t monotonic_time_ns()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

[[nodiscard]] int ioctl_retry(int fd, unsigned long request, void* argument)
{
    int result = 0;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

void print_usage(const char* program)
{
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "CachyOS USB-camera sender for validating the Aura UDP H.264 transport.\n"
        "The camera supplies MJPEG; this program decodes it, encodes low-delay\n"
        "H.264 with libx264, then sends complete access units through UDP.\n"
        "\n"
        "  --device PATH          V4L2 camera (default: /dev/video0)\n"
        "  --destination ADDRESS  UDP destination (default: 127.0.0.1)\n"
        "  --port PORT            UDP destination port (default: 5000)\n"
        "  --stream-id ID         Stream identifier (default: 0)\n"
        "  --width PIXELS         MJPEG capture width (default: 1280)\n"
        "  --height PIXELS        MJPEG capture height (default: 720)\n"
        "  --fps RATE             Capture rate (default: 60)\n"
        "  --frames COUNT         Frames to send (default: 120)\n"
        "  --bitrate-kbps RATE    H.264 target bitrate (default: 8000)\n",
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
    if (value > std::numeric_limits<Integer>::max()) {
        return false;
    }
    output = static_cast<Integer>(value);
    return true;
}

bool parse_options(int argc, char* argv[], Options& options)
{
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
        if (option == "--device") {
            options.device = value;
        } else if (option == "--destination") {
            options.destination = value;
        } else if (option == "--port") {
            if (!parse_integer(value, options.port) || options.port == 0U) {
                return false;
            }
        } else if (option == "--stream-id") {
            if (!parse_integer(value, options.stream_id)) {
                return false;
            }
        } else if (option == "--width") {
            if (!parse_integer(value, options.width) || options.width == 0U) {
                return false;
            }
        } else if (option == "--height") {
            if (!parse_integer(value, options.height) || options.height == 0U) {
                return false;
            }
        } else if (option == "--fps") {
            if (!parse_integer(value, options.frames_per_second) || options.frames_per_second == 0U) {
                return false;
            }
        } else if (option == "--frames") {
            if (!parse_integer(value, options.frame_count) || options.frame_count == 0U) {
                return false;
            }
        } else if (option == "--bitrate-kbps") {
            if (!parse_integer(value, options.bitrate_kbps) || options.bitrate_kbps == 0U) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

class V4l2MjpegCamera {
public:
    V4l2MjpegCamera(std::string_view device, std::uint32_t width, std::uint32_t height,
                    std::uint32_t frames_per_second)
    {
        try {
            const std::string path{device};
            fd_ = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd_ < 0) {
                throw std::runtime_error("could not open " + path + ": " + std::strerror(errno));
            }

            v4l2_capability capability{};
            if (ioctl_retry(fd_, VIDIOC_QUERYCAP, &capability) != 0) {
                throw std::runtime_error("VIDIOC_QUERYCAP failed: " + std::string{std::strerror(errno)});
            }
            if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0U ||
                (capability.capabilities & V4L2_CAP_STREAMING) == 0U) {
                throw std::runtime_error("V4L2 device does not support streaming video capture");
            }

            v4l2_format format{};
            format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            format.fmt.pix.width = width;
            format.fmt.pix.height = height;
            format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
            format.fmt.pix.field = V4L2_FIELD_NONE;
            if (ioctl_retry(fd_, VIDIOC_S_FMT, &format) != 0) {
                throw std::runtime_error("VIDIOC_S_FMT failed: " + std::string{std::strerror(errno)});
            }
            if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
                throw std::runtime_error("camera did not accept MJPEG capture format");
            }
            width_ = format.fmt.pix.width;
            height_ = format.fmt.pix.height;

            v4l2_streamparm stream_parameters{};
            stream_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            stream_parameters.parm.capture.timeperframe.numerator = 1U;
            stream_parameters.parm.capture.timeperframe.denominator = frames_per_second;
            if (ioctl_retry(fd_, VIDIOC_S_PARM, &stream_parameters) != 0) {
                throw std::runtime_error("VIDIOC_S_PARM failed: " + std::string{std::strerror(errno)});
            }

            v4l2_requestbuffers request{};
            request.count = 3U;
            request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            request.memory = V4L2_MEMORY_MMAP;
            if (ioctl_retry(fd_, VIDIOC_REQBUFS, &request) != 0 || request.count < 2U) {
                throw std::runtime_error("VIDIOC_REQBUFS could not allocate two capture buffers");
            }
            buffers_.resize(request.count);
            for (std::uint32_t index = 0U; index < request.count; ++index) {
                v4l2_buffer buffer{};
                buffer.type = request.type;
                buffer.memory = request.memory;
                buffer.index = index;
                if (ioctl_retry(fd_, VIDIOC_QUERYBUF, &buffer) != 0) {
                    throw std::runtime_error("VIDIOC_QUERYBUF failed: " + std::string{std::strerror(errno)});
                }
                buffers_[index].length = buffer.length;
                buffers_[index].data = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                            fd_, static_cast<off_t>(buffer.m.offset));
                if (buffers_[index].data == MAP_FAILED) {
                    buffers_[index].data = nullptr;
                    throw std::runtime_error("mmap capture buffer failed: " + std::string{std::strerror(errno)});
                }
                queue(index);
            }

            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl_retry(fd_, VIDIOC_STREAMON, &type) != 0) {
                throw std::runtime_error("VIDIOC_STREAMON failed: " + std::string{std::strerror(errno)});
            }
            streaming_ = true;
        } catch (...) {
            close();
            throw;
        }
    }

    ~V4l2MjpegCamera()
    {
        close();
    }

    V4l2MjpegCamera(const V4l2MjpegCamera&) = delete;
    V4l2MjpegCamera& operator=(const V4l2MjpegCamera&) = delete;

    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }

    [[nodiscard]] CapturedBuffer dequeue()
    {
        pollfd descriptor{.fd = fd_, .events = POLLIN, .revents = 0};
        const int ready = poll(&descriptor, 1U, 2000);
        if (ready == 0) {
            throw std::runtime_error("camera capture timed out");
        }
        if (ready < 0) {
            throw std::runtime_error("camera capture poll failed: " + std::string{std::strerror(errno)});
        }
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (ioctl_retry(fd_, VIDIOC_DQBUF, &buffer) != 0) {
            throw std::runtime_error("VIDIOC_DQBUF failed: " + std::string{std::strerror(errno)});
        }
        if (buffer.index >= buffers_.size() || buffer.bytesused > buffers_[buffer.index].length) {
            throw std::runtime_error("camera returned an invalid capture buffer");
        }
        return CapturedBuffer{
            .index = buffer.index,
            .data = {static_cast<const std::byte*>(buffers_[buffer.index].data), buffer.bytesused},
        };
    }

    void queue(std::uint32_t index)
    {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (ioctl_retry(fd_, VIDIOC_QBUF, &buffer) != 0) {
            throw std::runtime_error("VIDIOC_QBUF failed: " + std::string{std::strerror(errno)});
        }
    }

private:
    void close()
    {
        if (fd_ >= 0 && streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void)ioctl_retry(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for (const MappedBuffer& buffer : buffers_) {
            if (buffer.data != nullptr) {
                (void)munmap(buffer.data, buffer.length);
            }
        }
        buffers_.clear();
        if (fd_ >= 0) {
            (void)::close(fd_);
            fd_ = -1;
        }
    }

    int fd_{-1};
    bool streaming_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::vector<MappedBuffer> buffers_;
};

class MjpegDecoder {
public:
    MjpegDecoder()
    {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (codec == nullptr) {
            throw std::runtime_error("FFmpeg was built without the MJPEG decoder");
        }
        context_ = avcodec_alloc_context3(codec);
        frame_ = av_frame_alloc();
        if (context_ == nullptr || frame_ == nullptr) {
            throw std::runtime_error("could not allocate MJPEG decoder state");
        }
        const int result = avcodec_open2(context_, codec, nullptr);
        if (result < 0) {
            throw std::runtime_error("could not open MJPEG decoder: " + ffmpeg_error(result));
        }
    }

    ~MjpegDecoder()
    {
        av_frame_free(&frame_);
        avcodec_free_context(&context_);
    }

    [[nodiscard]] bool decode(std::span<const std::byte> mjpeg)
    {
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            throw std::runtime_error("could not allocate MJPEG packet");
        }
        const int allocation_result = av_new_packet(packet, static_cast<int>(mjpeg.size()));
        if (allocation_result < 0) {
            av_packet_free(&packet);
            throw std::runtime_error("could not allocate MJPEG payload: " + ffmpeg_error(allocation_result));
        }
        std::memcpy(packet->data, mjpeg.data(), mjpeg.size());
        const int send_result = avcodec_send_packet(context_, packet);
        av_packet_free(&packet);
        if (send_result < 0) {
            throw std::runtime_error("could not submit MJPEG frame: " + ffmpeg_error(send_result));
        }
        av_frame_unref(frame_);
        const int receive_result = avcodec_receive_frame(context_, frame_);
        if (receive_result == AVERROR(EAGAIN)) {
            return false;
        }
        if (receive_result < 0) {
            throw std::runtime_error("could not decode MJPEG frame: " + ffmpeg_error(receive_result));
        }
        return true;
    }

    [[nodiscard]] const AVFrame& frame() const { return *frame_; }

private:
    AVCodecContext* context_{};
    AVFrame* frame_{};
};

class H264Encoder {
public:
    H264Encoder(std::uint32_t width, std::uint32_t height, std::uint32_t frames_per_second,
                std::uint32_t bitrate_kbps)
    {
        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (codec == nullptr) {
            throw std::runtime_error("FFmpeg was built without the libx264 encoder");
        }
        context_ = avcodec_alloc_context3(codec);
        input_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (context_ == nullptr || input_ == nullptr || packet_ == nullptr) {
            throw std::runtime_error("could not allocate H.264 encoder state");
        }
        context_->width = static_cast<int>(width);
        context_->height = static_cast<int>(height);
        context_->pix_fmt = AV_PIX_FMT_YUV420P;
        context_->time_base = AVRational{1, static_cast<int>(frames_per_second)};
        context_->framerate = AVRational{static_cast<int>(frames_per_second), 1};
        context_->bit_rate = static_cast<std::int64_t>(bitrate_kbps) * 1000;
        context_->gop_size = static_cast<int>(frames_per_second / 2U);
        context_->max_b_frames = 0;
        context_->thread_count = 1;
        if (av_opt_set(context_->priv_data, "preset", "ultrafast", 0) < 0 ||
            av_opt_set(context_->priv_data, "tune", "zerolatency", 0) < 0 ||
            av_opt_set(context_->priv_data, "profile", "baseline", 0) < 0 ||
            av_opt_set(context_->priv_data, "x264-params",
                       "scenecut=0:rc-lookahead=0:sync-lookahead=0:repeat-headers=1:annexb=1", 0) < 0) {
            throw std::runtime_error("could not configure low-latency libx264 options");
        }
        const int open_result = avcodec_open2(context_, codec, nullptr);
        if (open_result < 0) {
            throw std::runtime_error("could not open libx264 encoder: " + ffmpeg_error(open_result));
        }

        input_->format = context_->pix_fmt;
        input_->width = context_->width;
        input_->height = context_->height;
        const int buffer_result = av_frame_get_buffer(input_, 32);
        if (buffer_result < 0) {
            throw std::runtime_error("could not allocate H.264 input frame: " + ffmpeg_error(buffer_result));
        }
    }

    ~H264Encoder()
    {
        sws_freeContext(scaler_);
        av_packet_free(&packet_);
        av_frame_free(&input_);
        avcodec_free_context(&context_);
    }

    void convert(const AVFrame& source, std::uint64_t frame_id)
    {
        AVPixelFormat source_format = static_cast<AVPixelFormat>(source.format);
        int source_range = 0;
        switch (source_format) {
        case AV_PIX_FMT_YUVJ420P:
            source_format = AV_PIX_FMT_YUV420P;
            source_range = 1;
            break;
        case AV_PIX_FMT_YUVJ422P:
            source_format = AV_PIX_FMT_YUV422P;
            source_range = 1;
            break;
        case AV_PIX_FMT_YUVJ444P:
            source_format = AV_PIX_FMT_YUV444P;
            source_range = 1;
            break;
        default:
            break;
        }
        scaler_ = sws_getCachedContext(
            scaler_, source.width, source.height, source_format,
            context_->width, context_->height, context_->pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (scaler_ == nullptr) {
            throw std::runtime_error("could not create YUV conversion context");
        }
        const int* coefficients = sws_getCoefficients(SWS_CS_ITU601);
        if (sws_setColorspaceDetails(scaler_, coefficients, source_range, coefficients, 0, 0, 1 << 16,
                                     1 << 16) < 0) {
            throw std::runtime_error("could not configure MJPEG YUV range conversion");
        }
        const int writable_result = av_frame_make_writable(input_);
        if (writable_result < 0) {
            throw std::runtime_error("could not make H.264 input frame writable: " +
                                     ffmpeg_error(writable_result));
        }
        const int converted = sws_scale(scaler_, source.data, source.linesize, 0, source.height,
                                        input_->data, input_->linesize);
        if (converted != context_->height) {
            throw std::runtime_error("could not convert MJPEG frame to YUV420");
        }
        input_->pts = static_cast<std::int64_t>(frame_id);
    }

    template <typename SendPacket>
    void encode(SendPacket&& send_packet)
    {
        const int send_result = avcodec_send_frame(context_, input_);
        if (send_result < 0) {
            throw std::runtime_error("could not submit H.264 frame: " + ffmpeg_error(send_result));
        }
        while (true) {
            const int receive_result = avcodec_receive_packet(context_, packet_);
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                break;
            }
            if (receive_result < 0) {
                throw std::runtime_error("could not encode H.264 frame: " + ffmpeg_error(receive_result));
            }
            send_packet(*packet_);
            av_packet_unref(packet_);
        }
    }

private:
    AVCodecContext* context_{};
    AVFrame* input_{};
    AVPacket* packet_{};
    SwsContext* scaler_{};
};

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    try {
        av_log_set_level(AV_LOG_ERROR);
        V4l2MjpegCamera camera{options.device, options.width, options.height, options.frames_per_second};
        MjpegDecoder mjpeg_decoder;
        H264Encoder h264_encoder{camera.width(), camera.height(), options.frames_per_second,
                                 options.bitrate_kbps};
        teleop::UdpSender sender{options.destination, options.port};
        teleop::TransportStatistics statistics;
        std::uint64_t encoded_access_units = 0U;

        for (std::uint32_t frame_id = 0U; frame_id < options.frame_count; ++frame_id) {
            const CapturedBuffer captured = camera.dequeue();
            const std::uint64_t capture_time_ns = monotonic_time_ns();
            try {
                if (mjpeg_decoder.decode(captured.data)) {
                    h264_encoder.convert(mjpeg_decoder.frame(), frame_id);
                    h264_encoder.encode([&](const AVPacket& packet) {
                        const auto flags = (packet.flags & AV_PKT_FLAG_KEY) != 0
                                               ? teleop::PacketFlags::kKeyFrame
                                               : teleop::PacketFlags::kNone;
                        const std::span<const std::byte> access_unit{
                            reinterpret_cast<const std::byte*>(packet.data),
                            static_cast<std::size_t>(packet.size)};
                        if (!sender.send_frame(options.stream_id, frame_id, flags, capture_time_ns,
                                               access_unit, statistics)) {
                            std::fprintf(stderr, "frame %u was not completely queued for UDP transmission\n",
                                         frame_id);
                        }
                        ++encoded_access_units;
                    });
                }
            } catch (...) {
                camera.queue(captured.index);
                throw;
            }
            camera.queue(captured.index);
        }

        std::printf("usb-camera-sender: frames=%u access_units=%llu datagrams=%llu bytes=%llu\n",
                    options.frame_count, static_cast<unsigned long long>(encoded_access_units),
                    static_cast<unsigned long long>(statistics.datagrams_sent),
                    static_cast<unsigned long long>(statistics.bytes_sent));
        return encoded_access_units == 0U ? EXIT_FAILURE : EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "usb-camera-sender: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
