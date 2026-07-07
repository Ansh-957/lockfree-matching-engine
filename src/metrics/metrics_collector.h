#pragma once

/// @file metrics_collector.h
/// @brief Aggregates latency histograms and counters for engine diagnostics.
///
/// Thread safety: The atomic counters are safe for concurrent access.
/// LatencyTracker methods are NOT thread-safe — call record_*_latency()
/// from the engine thread only.

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

private:
    LatencyTracker            match_latency_;       ///< Match operation latency histogram
    LatencyTracker            order_latency_;       ///< End-to-end order processing latency
    std::atomic<uint64_t>     total_orders_{0};     ///< Total orders processed
    std::atomic<uint64_t>     total_matches_{0};    ///< Total fills generated
};

} // namespace engine
