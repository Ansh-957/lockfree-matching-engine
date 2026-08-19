#pragma once

// HDR-style log-linear latency histogram.
//
// The problem with linear bins: match latency spans ~100ns to (rarely)
// milliseconds - four orders of magnitude. Linear 1us bins would dump every
// normal sample into bin 0 and report p50 == p99 == "under 1us", which is
// useless. Linear 1ns bins over the same range would need millions of bins.
//
// The HDR (High Dynamic Range) histogram trick: constant RELATIVE error
// instead of constant absolute error. Values are bucketed by
//   (power-of-two octave, 64 linear sub-buckets within the octave)
// so every bucket is at most 1/64 = 1.6% wide relative to its value.
// 100ns resolves to ~2ns, 100us to ~2us - the same precision in
// percentage terms, which is what percentile reporting actually needs.
//
// Recording is O(1) and branch-light: one bit_width() (a single LZCNT
// instruction), a shift, and two increments. No allocation ever.
// Percentile queries walk the ~2.4K buckets - O(buckets), done only by
// the metrics thread at reporting time, never on the hot path.
//
// Thread safety: none. This is deliberately a plain single-threaded
// structure owned by the metrics thread; samples arrive over an SPSC ring
// (see main.cpp). Making the bins atomic would put contended cache-line
// traffic back into the measurement path - the exact thing the ring
// buffer design avoids.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace engine {

class LatencyTracker {
public:
    // 64 sub-buckets per octave -> worst-case relative error 1/64 ~ 1.6%
    static constexpr unsigned SUB_BITS  = 6;
    static constexpr uint64_t SUB_COUNT = uint64_t{1} << SUB_BITS;

    // highest exactly-tracked value ~ 2^42 ns ~ 73 minutes; anything above
    // lands in the final (overflow) bucket. max() still reports it exactly.
    static constexpr unsigned MAX_BITS = 42;
    static constexpr size_t   NUM_BINS =
        SUB_COUNT + (MAX_BITS - SUB_BITS) * SUB_COUNT;  // 64 + 36*64 = 2368

    LatencyTracker() = default;

    void record(uint64_t ns) noexcept {
        size_t idx = bucket_index(ns);
        if (idx >= NUM_BINS) idx = NUM_BINS - 1;
        ++bins_[idx];
        ++count_;
        sum_ += ns;
        if (ns > max_) max_ = ns;
        if (ns < min_) min_ = ns;
    }

    // latency at the requested percentile, reported as the upper bound of
    // the bucket that contains it (i.e. "p99 <= this value", within 1.6%)
    [[nodiscard]] uint64_t percentile(double pct) const noexcept {
        if (count_ == 0) return 0;
        const auto target = static_cast<uint64_t>(
            (pct / 100.0) * static_cast<double>(count_) + 0.5);

        uint64_t cumulative = 0;
        for (size_t i = 0; i < NUM_BINS; ++i) {
            cumulative += bins_[i];
            if (cumulative >= target) return bucket_upper(i);
        }
        return max_;
    }

    [[nodiscard]] uint64_t p50()  const noexcept { return percentile(50.0); }
    [[nodiscard]] uint64_t p95()  const noexcept { return percentile(95.0); }
    [[nodiscard]] uint64_t p99()  const noexcept { return percentile(99.0); }
    [[nodiscard]] uint64_t p999() const noexcept { return percentile(99.9); }

    [[nodiscard]] uint64_t max()   const noexcept { return max_; }
    [[nodiscard]] uint64_t min()   const noexcept { return count_ ? min_ : 0; }
    [[nodiscard]] uint64_t count() const noexcept { return count_; }
    [[nodiscard]] uint64_t mean()  const noexcept {
        return count_ ? sum_ / count_ : 0;
    }

    void reset() noexcept {
        bins_.fill(0);
        count_ = 0;
        sum_   = 0;
        max_   = 0;
        min_   = UINT64_MAX;
    }

    // exposed for tests
    [[nodiscard]] static size_t bucket_index(uint64_t v) noexcept {
        if (v < SUB_COUNT) return static_cast<size_t>(v);  // 0..63: exact
        // msb >= 6. The top bit selects the octave; the next 6 bits below
        // it select the sub-bucket (the leading 1 is implicit, so subtract
        // SUB_COUNT to strip it)
        const unsigned msb   = static_cast<unsigned>(std::bit_width(v)) - 1;
        const unsigned shift = msb - SUB_BITS;
        const auto     sub   = static_cast<size_t>((v >> shift) - SUB_COUNT);
        return SUB_COUNT * (shift + 1) + sub;
    }

    [[nodiscard]] static uint64_t bucket_upper(size_t idx) noexcept {
        if (idx < SUB_COUNT) return idx;
        const size_t   oct = idx / SUB_COUNT - 1;
        const uint64_t sub = idx % SUB_COUNT;
        return ((SUB_COUNT + sub + 1) << oct) - 1;
    }

private:
    std::array<uint64_t, NUM_BINS> bins_{};  // ~18.5 KB, lives in the tracker
    uint64_t count_ = 0;
    uint64_t sum_   = 0;
    uint64_t max_   = 0;
    uint64_t min_   = UINT64_MAX;
};

} // namespace engine
