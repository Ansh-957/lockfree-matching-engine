/// @file metrics_collector.cpp
/// @brief Implementation of MetricsCollector::summary() and reset().

#include "metrics/metrics_collector.h"

#include <sstream>
#include <iomanip>

namespace engine {

std::string MetricsCollector::summary() const {
    std::ostringstream oss;

    oss << "========== Engine Metrics ==========\n";
    oss << "Total orders processed: " << total_orders_.load(std::memory_order_relaxed) << "\n";
    oss << "Total fills generated:  " << total_matches_.load(std::memory_order_relaxed) << "\n";
    oss << "\n";

    // Match latency stats
    oss << "--- Match Latency (ns) ---\n";
    oss << "  Samples: " << match_latency_.count() << "\n";
    if (match_latency_.count() > 0) {
        oss << "  p50:     " << std::setw(8) << match_latency_.p50()  << " ns\n";
        oss << "  p95:     " << std::setw(8) << match_latency_.p95()  << " ns\n";
        oss << "  p99:     " << std::setw(8) << match_latency_.p99()  << " ns\n";
        oss << "  p99.9:   " << std::setw(8) << match_latency_.p999() << " ns\n";
        oss << "  max:     " << std::setw(8) << match_latency_.max()  << " ns\n";
    }
    oss << "\n";

    // Order processing latency stats
    oss << "--- Order Latency (ns) ---\n";
    oss << "  Samples: " << order_latency_.count() << "\n";
    if (order_latency_.count() > 0) {
        oss << "  p50:     " << std::setw(8) << order_latency_.p50()  << " ns\n";
        oss << "  p95:     " << std::setw(8) << order_latency_.p95()  << " ns\n";
        oss << "  p99:     " << std::setw(8) << order_latency_.p99()  << " ns\n";
        oss << "  p99.9:   " << std::setw(8) << order_latency_.p999() << " ns\n";
        oss << "  max:     " << std::setw(8) << order_latency_.max()  << " ns\n";
    }

    oss << "====================================\n";

    return oss.str();
}

void MetricsCollector::reset() noexcept {
    match_latency_.reset();
    order_latency_.reset();
    total_orders_.store(0, std::memory_order_relaxed);
    total_matches_.store(0, std::memory_order_relaxed);
}

} // namespace engine
