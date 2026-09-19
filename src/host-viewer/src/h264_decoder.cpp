#include "h264_decoder.hpp"

#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace teleop {
namespace {

std::string ffmpeg_error(int error_code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    av_strerror(error_code, message.data(), message.size());
    return {message.data()};
}

AVPixelFormat select_vaapi_format(AVCodecContext*, const AVPixelFormat* formats)
{
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_VAAPI) {
            return *format;
        }
    }
    std::fputs("host-viewer: H.264 decoder did not offer a VAAPI output format\n", stderr);
    return AV_PIX_FMT_NONE;
}

} // namespace

H264VaapiDecoder::H264VaapiDecoder(std::string_view vaapi_device)
{
    try {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (codec == nullptr) {
            throw std::runtime_error("FFmpeg was built without the H.264 decoder");
        }

        const std::string device_path{vaapi_device};
        const int device_result = av_hwdevice_ctx_create(
            &hardware_device_, AV_HWDEVICE_TYPE_VAAPI,
            device_path.empty() ? nullptr : device_path.c_str(), nullptr, 0);
        if (device_result < 0) {
            throw std::runtime_error("could not create VAAPI device " + device_path + ": " +
                                     ffmpeg_error(device_result));
        }

        context_ = avcodec_alloc_context3(codec);
        if (context_ == nullptr) {
            throw std::runtime_error("could not allocate H.264 decoder context");
        }
        context_->hw_device_ctx = av_buffer_ref(hardware_device_);
        if (context_->hw_device_ctx == nullptr) {
            throw std::runtime_error("could not retain VAAPI device context");
        }
        context_->get_format = select_vaapi_format;
        context_->thread_count = 1;

        const int open_result = avcodec_open2(context_, codec, nullptr);
        if (open_result < 0) {
            throw std::runtime_error("could not open VAAPI H.264 decoder: " +
                                     ffmpeg_error(open_result));
        }
    } catch (...) {
        avcodec_free_context(&context_);
        av_buffer_unref(&hardware_device_);
        throw;
    }
}

H264VaapiDecoder::~H264VaapiDecoder()
{
    avcodec_free_context(&context_);
    av_buffer_unref(&hardware_device_);
}

void H264VaapiDecoder::drain()
{
    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        throw std::runtime_error("could not allocate decoded frame");
    }

    while (true) {
        const int result = avcodec_receive_frame(context_, frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        }
        if (result < 0) {
            av_frame_free(&frame);
            throw std::runtime_error("VAAPI H.264 decode failed: " + ffmpeg_error(result));
        }
        if (frame->format != AV_PIX_FMT_VAAPI) {
            av_frame_free(&frame);
            throw std::runtime_error("decoder returned a software frame; VAAPI fallback is disabled");
        }
        ++statistics_.decoded_frames;
        ++statistics_.hardware_frames;
        av_frame_unref(frame);
    }
    av_frame_free(&frame);
}

void H264VaapiDecoder::submit_access_unit(std::span<const std::byte> access_unit)
{
    if (access_unit.empty()) {
        return;
    }

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        throw std::runtime_error("could not allocate encoded packet");
    }
    const int allocation_result = av_new_packet(packet, static_cast<int>(access_unit.size()));
    if (allocation_result < 0) {
        av_packet_free(&packet);
        throw std::runtime_error("could not allocate encoded packet payload: " +
                                 ffmpeg_error(allocation_result));
    }
    std::memcpy(packet->data, access_unit.data(), access_unit.size());

    int send_result = avcodec_send_packet(context_, packet);
    if (send_result == AVERROR(EAGAIN)) {
        drain();
        send_result = avcodec_send_packet(context_, packet);
    }
    av_packet_free(&packet);
    if (send_result < 0) {
        throw std::runtime_error("could not submit H.264 access unit: " + ffmpeg_error(send_result));
    }
    drain();
}

void H264VaapiDecoder::flush()
{
    int send_result = avcodec_send_packet(context_, nullptr);
    if (send_result == AVERROR(EAGAIN)) {
        drain();
        send_result = avcodec_send_packet(context_, nullptr);
    }
    if (send_result < 0 && send_result != AVERROR_EOF) {
        throw std::runtime_error("could not flush H.264 decoder: " + ffmpeg_error(send_result));
    }
    drain();
}

const DecodeStatistics& H264VaapiDecoder::statistics() const
{
    return statistics_;
}

AVCodecContext* H264VaapiDecoder::codec_context() const
{
    return context_;
}

} // namespace teleop
