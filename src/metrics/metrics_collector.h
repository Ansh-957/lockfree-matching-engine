#pragma once

/// @file metrics_collector.h
/// @brief Aggregates latency histograms and counters for engine diagnostics.
///
/// Ownership model (Commit 11): the collector lives entirely on the
/// metrics/output thread. The engine thread never touches it - it ships
/// raw nanosecond samples over an SPSC ring and this thread does all the
/// histogram work. The atomic counters are kept atomic only so stray
/// cross-thread reads (stats lines) stay defined behavior.

#include <atomic>
#include <cstdint>
#include <string>

#include "metrics/latency_tracker.h"

namespace engine {

/// @brief Collects matching engine performance metrics.
class MetricsCollector {
public:
    MetricsCollector() = default;

    // -------------------------------------------------------------------
    // Recording (called from engine thread)
    // -------------------------------------------------------------------

    /// @brief Record the latency of a matching operation.
    void record_match_latency(uint64_t ns) noexcept {
        match_latency_.record(ns);
    }

    /// @brief Record the end-to-end latency of processing an order.
    void record_order_latency(uint64_t ns) noexcept {
        order_latency_.record(ns);
    }

    /// @brief Increment the total order count.
    void increment_orders() noexcept {
        total_orders_.fetch_add(1, std::memory_order_relaxed);
    }

    /// @brief Increment the total match/fill count.
    void increment_matches() noexcept {
        total_matches_.fetch_add(1, std::memory_order_relaxed);
    }

    /// @brief Count telemetry samples lost because the sample ring was full.
    void note_dropped_samples(uint64_t n) noexcept {
        dropped_samples_.fetch_add(n, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------
    // Reporting
    // -------------------------------------------------------------------

    /// @brief Generate a human-readable summary of all metrics.
    /// @return Formatted multi-line string with p50/p95/p99/max stats.
    [[nodiscard]] std::string summary() const;

    /// @brief Reset all metrics to zero.
    void reset() noexcept;

    // -------------------------------------------------------------------
    // Individual accessors
    // -------------------------------------------------------------------

    [[nodiscard]] const LatencyTracker& match_latency() const noexcept {
        return match_latency_;
    }

    [[nodiscard]] const LatencyTracker& order_latency() const noexcept {
        return order_latency_;
    }

    [[nodiscard]] uint64_t total_orders() const noexcept {
        return total_orders_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t total_matches() const noexcept {
        return total_matches_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t dropped_samples() const noexcept {
        return dropped_samples_.load(std::memory_order_relaxed);
    }

private:
    LatencyTracker            match_latency_;       ///< Time inside MatchingEngine::process
    LatencyTracker            order_latency_;       ///< Ingest push -> engine done (queue wait + match)
    std::atomic<uint64_t>     total_orders_{0};     ///< Total orders processed
    std::atomic<uint64_t>     total_matches_{0};    ///< Total fills generated
    std::atomic<uint64_t>     dropped_samples_{0};  ///< Telemetry ring overflows
};

} // namespace engine
