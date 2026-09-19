#include <teleop/statistics.hpp>

#include <algorithm>

namespace teleop {

void LatencyStatistics::add_sample(std::uint64_t nanoseconds)
{
    if (count_ == 0U) {
        minimum_ns_ = nanoseconds;
        maximum_ns_ = nanoseconds;
    } else {
        minimum_ns_ = std::min(minimum_ns_, nanoseconds);
        maximum_ns_ = std::max(maximum_ns_, nanoseconds);
    }
    ++count_;
    total_ns_ += static_cast<long double>(nanoseconds);
}

std::uint64_t LatencyStatistics::sample_count() const
{
    return count_;
}

std::optional<double> LatencyStatistics::minimum_ms() const
{
    if (count_ == 0U) {
        return std::nullopt;
    }
    return static_cast<double>(minimum_ns_) / 1'000'000.0;
}

std::optional<double> LatencyStatistics::average_ms() const
{
    if (count_ == 0U) {
        return std::nullopt;
    }
    return static_cast<double>(total_ns_ / static_cast<long double>(count_)) / 1'000'000.0;
}

std::optional<double> LatencyStatistics::maximum_ms() const
{
    if (count_ == 0U) {
        return std::nullopt;
    }
    return static_cast<double>(maximum_ns_) / 1'000'000.0;
}

} // namespace teleop
