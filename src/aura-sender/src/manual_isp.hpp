#pragma once

// Minimal static ISP35 parameters. Own the metadata queue for the camera lifetime;
// stop VI before closing it. No 3A thread, frame copies or video buffering added.
#include <linux/media.h>
#include <linux/videodev2.h>
#include <linux/rk-isp35-config.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace teleop::manual_isp {
struct Settings {
    bool wb{}, bls{};
    std::array<std::uint16_t, 4> gains{256, 256, 256, 256}; // R/Gr/Gb/B, Q8
    std::array<std::int16_t, 4> black{}; // ISP BLS register units, not RAW10 DN
    bool enabled() const { return wb || bls; }
};

inline bool parse_tuple(std::string_view text, std::array<float, 4>& values)
{
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto comma = text.find(',');
        if ((i < 3) != (comma != std::string_view::npos)) return false;
        const auto part = text.substr(0, comma);
        const auto result = std::from_chars(part.data(), part.data() + part.size(), values[i]);
        if (result.ec != std::errc{} || result.ptr != part.data() + part.size() || !std::isfinite(values[i])) return false;
        if (i < 3) text.remove_prefix(comma + 1);
    }
    return true;
}

inline bool set_wb(Settings& settings, std::string_view text)
{
    std::array<float, 4> values{};
    if (!parse_tuple(text, values)) return false;
    for (const auto value : values) if (value < 0.25F || value > 8.0F) return false;
    for (std::size_t i = 0; i < 4; ++i)
        settings.gains[i] = static_cast<std::uint16_t>(std::lround(values[i] * 256.0F));
    settings.wb = true;
    return true;
}

inline bool set_black(Settings& settings, std::string_view text)
{
    std::array<float, 4> values{};
    if (!parse_tuple(text, values)) return false;
    for (const auto value : values) if (value < 0 || value > 4095 || std::floor(value) != value) return false;
    for (std::size_t i = 0; i < 4; ++i) settings.black[i] = static_cast<std::int16_t>(values[i]);
    settings.bls = true;
    return true;
}

inline isp35_isp_params_cfg make_params(const Settings& settings)
{
    isp35_isp_params_cfg p{};
    if (settings.wb) {
        p.module_en_update |= ISP35_MODULE_AWB_GAIN;
        p.module_cfg_update |= ISP35_MODULE_AWB_GAIN;
        p.module_ens |= ISP35_MODULE_AWB_GAIN;
        auto& w = p.others.awb_gain_cfg;
        w.gain0_red = w.gain1_red = w.gain2_red = settings.gains[0];
        w.gain0_green_r = w.gain1_green_r = w.gain2_green_r = settings.gains[1];
        w.gain0_green_b = w.gain1_green_b = w.gain2_green_b = settings.gains[2];
        w.gain0_blue = w.gain1_blue = w.gain2_blue = settings.gains[3];
        // Second gain stage stays at unity to avoid applying WB twice.
        w.awb1_gain_r = w.awb1_gain_gr = w.awb1_gain_gb = w.awb1_gain_b = 256;
    }
    if (settings.bls) {
        p.module_en_update |= ISP35_MODULE_BLS;
        p.module_cfg_update |= ISP35_MODULE_BLS;
        p.module_ens |= ISP35_MODULE_BLS;
        auto& b = p.others.bls_cfg;
        b.fixed_val.r = settings.black[0]; b.fixed_val.gr = settings.black[1];
        b.fixed_val.gb = settings.black[2]; b.fixed_val.b = settings.black[3];
        b.isp_ob_max = 0xfffff;
    }
    return p;
}

inline std::string params_node(int camera)
{
    const auto model = "rkisp" + std::to_string(camera);
    for (int index = 0; index < 64; ++index) {
        const auto path = "/dev/media" + std::to_string(index);
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        media_device_info info{};
        if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) == 0 && model == info.model) {
            media_entity_desc entity{};
            entity.id = MEDIA_ENT_ID_FLAG_NEXT;
            while (ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &entity) == 0) {
                if (std::strcmp(entity.name, "rkisp-input-params") == 0) {
                    const auto sys = "/sys/dev/char/" + std::to_string(entity.dev.major) + ":" + std::to_string(entity.dev.minor);
                    close(fd);
                    return "/dev/" + std::filesystem::canonical(sys).filename().string();
                }
                entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
            }
        }
        close(fd);
    }
    throw std::runtime_error("No ISP params entity for " + model);
}

class Session {
public:
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() { close(); }
    void prepare(int camera, const Settings& settings)
    {
        if (!settings.enabled()) return;
        camera_ = camera;
        settings_ = settings;
        const auto node = params_node(camera);
        fd_ = open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) fail("open params");
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_META_OUTPUT;
        if (ioctl(fd_, VIDIOC_G_FMT, &fmt) < 0 || fmt.fmt.meta.buffersize != sizeof(isp35_isp_params_cfg))
            fail("ISP35 ABI mismatch");
        v4l2_requestbuffers req{};
        req.count = 2; req.type = V4L2_BUF_TYPE_META_OUTPUT; req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count != 2) fail("params buffers");
        const auto params = make_params(settings);
        for (unsigned i = 0; i < 2; ++i) {
            v4l2_buffer buf{};
            buf.type = req.type; buf.memory = req.memory; buf.index = i;
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0 || buf.length < sizeof(params)) fail("query params buffer");
            lengths_[i] = buf.length;
            maps_[i] = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
            if (maps_[i] == MAP_FAILED) { maps_[i] = nullptr; fail("map params"); }
            std::memcpy(maps_[i], &params, sizeof(params));
            buf.bytesused = static_cast<__u32>(sizeof(params));
            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) fail("queue params");
        }
        auto type = V4L2_BUF_TYPE_META_OUTPUT;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) fail("start params queue");
        streaming_ = true;
        std::fprintf(stderr, "isp: camera=%d params=%s wb=%d gains_q8=%u,%u,%u,%u bls=%d black_isp=%d,%d,%d,%d\n",
                     camera, node.c_str(), settings.wb, settings.gains[0], settings.gains[1], settings.gains[2], settings.gains[3],
                     settings.bls, settings.black[0], settings.black[1], settings.black[2], settings.black[3]);
    }
    void verify()
    {
        if (fd_ < 0 || verified_) return;
        std::ifstream file("/proc/rkisp-vir" + std::to_string(camera_));
        const std::string state{std::istreambuf_iterator<char>{file}, {}};
        if (settings_.wb) {
            std::smatch m;
            const std::regex pattern{R"(AWBGAIN\s+ON[^\n]*gain0:0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+))"};
            if (!std::regex_search(state, m, pattern)) fail("WB not enabled in procfs");
            const auto green = std::stoul(m[1].str(), nullptr, 16);
            const auto rb = std::stoul(m[2].str(), nullptr, 16);
            if ((green >> 16) != settings_.gains[1] || (green & 0xffff) != settings_.gains[2] ||
                (rb >> 16) != settings_.gains[0] || (rb & 0xffff) != settings_.gains[3]) fail("WB readback mismatch");
        }
        if (settings_.bls && !std::regex_search(state, std::regex{R"(BLS\s+ON\()"})) fail("BLS not enabled in procfs");
        std::fprintf(stderr, "isp: camera=%d procfs WB gain/enable readback passed; BLS fixed values require diagnostic register snapshot\n", camera_);
        verified_ = true;
    }
    bool close() noexcept
    {
        bool ok = true;
        if (streaming_) {
            auto type = V4L2_BUF_TYPE_META_OUTPUT;
            if (ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) { std::perror("stop ISP params"); ok = false; }
            streaming_ = false;
        }
        for (unsigned i = 0; i < 2; ++i) {
            if (maps_[i]) { munmap(maps_[i], lengths_[i]); maps_[i] = nullptr; }
        }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        return ok;
    }
private:
    [[noreturn]] static void fail(const char* what) { throw std::runtime_error(std::string{"ISP: "} + what); }
    int fd_{-1}, camera_{};
    bool streaming_{}, verified_{};
    Settings settings_{};
    std::array<void*, 2> maps_{};
    std::array<std::size_t, 2> lengths_{};
};
} // namespace teleop::manual_isp
