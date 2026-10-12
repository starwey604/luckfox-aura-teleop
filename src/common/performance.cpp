#include <teleop/performance.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <time.h>

namespace teleop {
namespace {

std::size_t bin_index(std::uint64_t value)
{
    if (value < 64U) return static_cast<std::size_t>(value);
    const unsigned int exponent = static_cast<unsigned int>(std::bit_width(value) - 1U);
    const auto fraction = (value - (std::uint64_t{1} << exponent)) >> (exponent - 6U);
    return static_cast<std::size_t>((exponent - 5U) * 64U + fraction);
}

std::uint64_t bin_upper(std::size_t index)
{
    if (index < 64U) return static_cast<std::uint64_t>(index);
    const unsigned int exponent = static_cast<unsigned int>(index / 64U + 5U);
    if (exponent == 63U && index % 64U == 63U) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (std::uint64_t{1} << exponent) +
           (static_cast<std::uint64_t>(index % 64U) + 1U) *
               (std::uint64_t{1} << (exponent - 6U)) - 1U;
}

auto& stages()
{
    thread_local std::map<std::string, DurationHistogram, std::less<>> result;
    return result;
}

} // namespace

void DurationHistogram::add(std::uint64_t nanoseconds)
{
    if (count_ == 0U) minimum_ = nanoseconds;
    minimum_ = std::min(minimum_, nanoseconds);
    maximum_ = std::max(maximum_, nanoseconds);
    total_ += static_cast<long double>(nanoseconds);
    ++count_;
    ++bins_[bin_index(nanoseconds)];
}

std::optional<std::uint64_t> DurationHistogram::percentile(double fraction) const
{
    if (count_ == 0U || !std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) {
        return std::nullopt;
    }
    if (fraction == 0.0) return minimum_;
    if (fraction == 1.0) return maximum_;
    const auto rank = std::max(std::uint64_t{1}, static_cast<std::uint64_t>(
        std::ceil(static_cast<long double>(fraction) * static_cast<long double>(count_))));
    std::uint64_t cumulative = 0U;
    for (std::size_t index = 0U; index < bins_.size(); ++index) {
        cumulative += bins_[index];
        if (cumulative >= rank) return std::min(bin_upper(index), maximum_);
    }
    return maximum_;
}

double DurationHistogram::mean_ns() const
{
    return count_ == 0U ? 0.0 : static_cast<double>(total_ / static_cast<long double>(count_));
}

bool software_metrics_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("TELEOP_METRICS");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled;
}

std::uint64_t performance_time_ns()
{
    if (!software_metrics_enabled()) return 0U;
    timespec timestamp{};
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) return 0U;
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1'000'000'000U +
           static_cast<std::uint64_t>(timestamp.tv_nsec);
}

void record_stage(std::string_view name, std::uint64_t begin, std::uint64_t end)
{
    if (begin == 0U || end < begin || !software_metrics_enabled()) return;
    auto& registry = stages();
    auto found = registry.find(name);
    if (found == registry.end()) found = registry.try_emplace(std::string{name}).first;
    found->second.add(end - begin);
}

void print_software_metrics(std::string_view prefix)
{
    if (!software_metrics_enabled()) return;
    std::puts("software-metrics: CLOCK_MONOTONIC local-only; ms; quantiles=upper-bin-bounds (width<=1.563%)");
    for (const auto& [name, histogram] : stages()) {
        std::printf("metric: stage=%.*s%s samples=%llu min=%.6f mean=%.6f p50=%.6f p95=%.6f p99=%.6f max=%.6f\n",
            static_cast<int>(prefix.size()), prefix.data() ? prefix.data() : "", name.c_str(),
            static_cast<unsigned long long>(histogram.count()),
            static_cast<double>(histogram.minimum()) / 1'000'000.0,
            histogram.mean_ns() / 1'000'000.0,
            static_cast<double>(*histogram.percentile(0.50)) / 1'000'000.0,
            static_cast<double>(*histogram.percentile(0.95)) / 1'000'000.0,
            static_cast<double>(*histogram.percentile(0.99)) / 1'000'000.0,
            static_cast<double>(histogram.maximum()) / 1'000'000.0);
    }
}

} // namespace teleop
