#pragma once

/// @file latency_tracker.h
/// @brief Fixed-bin histogram for tracking latency distributions.
///
/// Provides O(1) recording and O(N_bins) percentile queries. Designed for
/// nanosecond-granularity measurements in the matching engine hot path.
///
/// Bin layout:
///   - Bins 0 through NUM_BINS-1 each cover 1 microsecond of latency.
///   - Bin[i] counts samples with latency in [i*BIN_WIDTH_NS, (i+1)*BIN_WIDTH_NS).
///   - Any sample >= NUM_BINS * BIN_WIDTH_NS goes into the overflow bin (last bin).
///
/// This gives coverage from 0 to 1ms at 1μs resolution (1000 bins),
/// which is appropriate for sub-millisecond matching engine latencies.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace engine {

/// @brief A fixed-bin histogram for latency measurement in nanoseconds.
class LatencyTracker {
public:
    /// Number of histogram bins.
    static constexpr size_t NUM_BINS = 1000;

    /// Width of each bin in nanoseconds (1 microsecond).
    static constexpr uint64_t BIN_WIDTH_NS = 1000;

    /// Maximum trackable latency before overflow (1 millisecond).
    static constexpr uint64_t MAX_TRACKED_NS = NUM_BINS * BIN_WIDTH_NS;

    LatencyTracker() {
        reset();
    }

    /// @brief Record a latency sample.
    /// @param nanoseconds  The latency to record, in nanoseconds.
    void record(uint64_t nanoseconds) noexcept {
        size_t bin = static_cast<size_t>(nanoseconds / BIN_WIDTH_NS);
        if (bin >= NUM_BINS) {
            bin = NUM_BINS - 1;  // Overflow into last bin
        }

        ++bins_[bin];
        ++count_;

        if (nanoseconds > max_) {
            max_ = nanoseconds;
        }
    }

    /// @brief Get the p-th percentile latency.
    /// @param percentile  Value in (0.0, 100.0], e.g. 50.0 for p50.
    /// @return The latency (in ns) at the upper bound of the bin containing
    ///         the requested percentile, or 0 if no samples recorded.
    [[nodiscard]] uint64_t percentile(double percentile) const noexcept {
        if (count_ == 0) return 0;

        // Number of samples at or below the target percentile.
        const uint64_t target = static_cast<uint64_t>(
            (percentile / 100.0) * static_cast<double>(count_));

        uint64_t cumulative = 0;
        for (size_t i = 0; i < NUM_BINS; ++i) {
            cumulative += bins_[i];
            if (cumulative >= target) {
                return (i + 1) * BIN_WIDTH_NS;
            }
        }

        return MAX_TRACKED_NS;  // All samples are in the overflow bin
    }

    /// @brief Median latency (p50).
    [[nodiscard]] uint64_t p50() const noexcept { return percentile(50.0); }

    /// @brief 95th percentile latency.
    [[nodiscard]] uint64_t p95() const noexcept { return percentile(95.0); }

    /// @brief 99th percentile latency.
    [[nodiscard]] uint64_t p99() const noexcept { return percentile(99.0); }

    /// @brief 99.9th percentile latency.
    [[nodiscard]] uint64_t p999() const noexcept { return percentile(99.9); }

    /// @brief Maximum recorded latency.
    [[nodiscard]] uint64_t max() const noexcept { return max_; }

    /// @brief Total number of recorded samples.
    [[nodiscard]] uint64_t count() const noexcept { return count_; }

    /// @brief Reset all bins and counters to zero.
    void reset() noexcept {
        std::memset(bins_.data(), 0, sizeof(bins_));
        count_ = 0;
        max_   = 0;
    }

private:
    std::array<uint64_t, NUM_BINS> bins_{};  ///< Histogram bins
    uint64_t count_ = 0;                     ///< Total samples recorded
    uint64_t max_   = 0;                     ///< Maximum observed latency (ns)
};

} // namespace engine
