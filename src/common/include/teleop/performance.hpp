#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace teleop {

// Fixed memory, all observations retained as counts. Quantiles are upper bin
// bounds with <= 1/64 relative width (or one nanosecond for small values).
class DurationHistogram {
public:
    void add(std::uint64_t nanoseconds);
    [[nodiscard]] std::optional<std::uint64_t> percentile(double fraction) const;
    [[nodiscard]] std::uint64_t count() const { return count_; }
    [[nodiscard]] std::uint64_t minimum() const { return minimum_; }
    [[nodiscard]] std::uint64_t maximum() const { return maximum_; }
    [[nodiscard]] double mean_ns() const;

private:
    std::array<std::uint64_t, 3776U> bins_{};
    std::uint64_t count_{};
    std::uint64_t minimum_{};
    std::uint64_t maximum_{};
    long double total_{};
};

// Opt-in, process-local instrumentation; never subtract board and host clocks.
// Histograms belong to the calling thread; print from each worker before joining.
[[nodiscard]] bool software_metrics_enabled();
[[nodiscard]] std::uint64_t performance_time_ns(); // zero while disabled
void record_stage(std::string_view name, std::uint64_t begin, std::uint64_t end);
void print_software_metrics(std::string_view prefix = {});

} // namespace teleop
