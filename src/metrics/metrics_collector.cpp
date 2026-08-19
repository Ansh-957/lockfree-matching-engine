/// @file metrics_collector.cpp
/// @brief Implementation of MetricsCollector::summary() and reset().

#include "metrics/metrics_collector.h"

#include <iomanip>
#include <sstream>

namespace engine {

namespace {

void print_tracker(std::ostringstream& oss, const char* title,
                   const LatencyTracker& t) {
    oss << "--- " << title << " (ns) ---\n";
    oss << "  samples: " << t.count() << "\n";
    if (t.count() == 0) return;
    oss << "  min:     " << std::setw(10) << t.min()  << "\n"
        << "  mean:    " << std::setw(10) << t.mean() << "\n"
        << "  p50:     " << std::setw(10) << t.p50()  << "\n"
        << "  p95:     " << std::setw(10) << t.p95()  << "\n"
        << "  p99:     " << std::setw(10) << t.p99()  << "\n"
        << "  p99.9:   " << std::setw(10) << t.p999() << "\n"
        << "  max:     " << std::setw(10) << t.max()  << "\n";
}

} // namespace

std::string MetricsCollector::summary() const {
    std::ostringstream oss;

    oss << "========== Engine Metrics ==========\n";
    oss << "orders processed: " << total_orders() << "\n";
    oss << "fills generated:  " << total_matches() << "\n";
    if (dropped_samples() > 0) {
        oss << "telemetry samples dropped (ring full): " << dropped_samples() << "\n";
    }
    oss << "\n";

    print_tracker(oss, "Match latency: inside process()", match_latency_);
    oss << "\n";
    print_tracker(oss, "Order latency: ingest -> engine done", order_latency_);

    oss << "====================================\n";
    return oss.str();
}

void MetricsCollector::reset() noexcept {
    match_latency_.reset();
    order_latency_.reset();
    total_orders_.store(0, std::memory_order_relaxed);
    total_matches_.store(0, std::memory_order_relaxed);
    dropped_samples_.store(0, std::memory_order_relaxed);
}

} // namespace engine
