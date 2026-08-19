#pragma once

// Nanosecond clock built on the x86 Time Stamp Counter.
//
// Why not std::chrono::steady_clock on the hot path? steady_clock resolves
// to clock_gettime(CLOCK_MONOTONIC), ~20-25ns per call even through the
// vDSO. We read the clock twice per message; at ~6M msgs/s that is a ~25%
// throughput tax. RDTSC is a single unprivileged instruction (~7ns) that
// reads a 64-bit counter incrementing at a fixed rate.
//
// "Fixed rate" is the invariant-TSC guarantee on every x86 CPU from the
// last ~15 years: the counter ticks at a constant frequency regardless of
// turbo/power states, and is synchronized across cores by hardware+kernel.
// We still have to learn that frequency at runtime - there is no portable
// register for it - so calibrate() measures how many TSC ticks elapse over
// a steady_clock interval and derives ns-per-tick once at startup.
//
// Accuracy note: RDTSC is not a serializing instruction; the CPU may
// reorder it slightly against neighboring loads/stores. For histogram
// telemetry (not for ordering decisions) that jitter of a few ns is
// irrelevant and not worth the pipeline flush of RDTSCP/LFENCE.
//
// Non-x86 fallback: steady_clock, correct just slower.

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define ENGINE_HAS_TSC 1
#else
#include <chrono>
#define ENGINE_HAS_TSC 0
#endif

#if ENGINE_HAS_TSC
#include <chrono>
#include <thread>
#endif

namespace engine::tsc {

#if ENGINE_HAS_TSC

namespace detail {
// ns per TSC tick, fixed after first call. double keeps the multiply
// exact enough (53-bit mantissa) for any realistic uptime.
inline double calibrate_ns_per_tick() {
    using clock = std::chrono::steady_clock;
    const auto     t0 = clock::now();
    const uint64_t c0 = __rdtsc();
    // 20ms is long enough that the two clock reads' own latency (~50ns)
    // is noise: 50ns / 20ms = 2.5e-6 relative error
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto     t1 = clock::now();
    const uint64_t c1 = __rdtsc();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return static_cast<double>(ns) / static_cast<double>(c1 - c0);
}

inline double ns_per_tick() {
    static const double v = calibrate_ns_per_tick();  // thread-safe magic static
    return v;
}
} // namespace detail

// call once at startup so the 20ms calibration sleep does not happen
// lazily in the middle of the first timed message
inline void calibrate() { (void)detail::ns_per_tick(); }

// monotonic nanoseconds since an arbitrary epoch (differences only)
inline uint64_t now_ns() {
    return static_cast<uint64_t>(static_cast<double>(__rdtsc()) * detail::ns_per_tick());
}

#else

inline void calibrate() {}

inline uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

#endif

} // namespace engine::tsc
