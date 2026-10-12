#include <teleop/performance.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

int main()
{
    teleop::DurationHistogram histogram;
    if (histogram.percentile(0.5).has_value()) return 1;
    // Exercise every binary boundary, including the UINT64_MAX overflow bin.
    for (unsigned int power = 0U; power < 64U; ++power) {
        for (const auto value : { (std::uint64_t{1} << power) - 1U,
                                  std::uint64_t{1} << power }) {
            teleop::DurationHistogram single;
            single.add(value);
            if (*single.percentile(0.5) != value) return 2;
        }
    }
    histogram.add(std::numeric_limits<std::uint64_t>::max());
    if (*histogram.percentile(0.99) != std::numeric_limits<std::uint64_t>::max()) return 3;
    teleop::DurationHistogram distribution;
    for (std::uint64_t value = 1U; value <= 10000U; ++value) distribution.add(value * 1000U);
    for (const auto fraction : {0.50, 0.95, 0.99}) {
        const auto exact = static_cast<std::uint64_t>(std::ceil(fraction * 10000.0)) * 1000U;
        const auto bound = *distribution.percentile(fraction);
        if (bound < exact || bound - exact > exact / 64U + 1U) return 4;
    }
    if (distribution.minimum() != 1000U || distribution.maximum() != 10000000U ||
        std::abs(distribution.mean_ns() - 5000500.0) > 0.01 ||
        distribution.percentile(-0.1).has_value() ||
        distribution.percentile(std::numeric_limits<double>::quiet_NaN()).has_value()) return 5;
    std::puts("performance histogram: rank, bounds, empty data and integer extremes passed");
    return 0;
}
