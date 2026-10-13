#pragma once

#include <array>
#include <string_view>
#include <sys/resource.h>

namespace teleop {
struct ViewerThreadPolicy {
    int rx_cpu{-1};
    std::array<int, 2U> decoder_cpu{-1, -1};
    int rx_rr_priority{};
};

// Only configures the calling worker. Default values preserve inherited policy
// and affinity. An explicitly requested setting that cannot be applied throws.
void configure_worker(std::string_view role, int cpu, int rr_priority = 0);

class CpuLatencyRequest {
public:
    explicit CpuLatencyRequest(int microseconds);
    ~CpuLatencyRequest();
    CpuLatencyRequest(const CpuLatencyRequest&) = delete;
    CpuLatencyRequest& operator=(const CpuLatencyRequest&) = delete;
private:
    int fd_{-1};
};

class ThreadObservation {
public:
    explicit ThreadObservation(std::string_view role) noexcept;
    ~ThreadObservation();
private:
    std::string_view role_;
    rusage before_{};
    long long migrations_before_{-1};
    bool usage_valid_{};
};
} // namespace teleop
