/// @file main.cpp
/// @brief Entry point for the matching engine application.
///
/// Architecture overview:
///   Thread 1 (Feed):   WebSocket → FeedHandler → SPSC Queue (producer)
///   Thread 2 (Engine): SPSC Queue (consumer) → MatchingEngine → Output Queue
///   Thread 3 (Output): Output Queue → TradeLogger, MetricsCollector
///
/// In synthetic mode, Thread 1 generates random orders instead of connecting
/// to a live feed. This is used for benchmarking and testing.

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

// Core engine
#include "core/types.h"
#include "core/matching_engine.h"

// Transport
#include "transport/spsc_queue.h"
#include "transport/message.h"

// Metrics
#include "metrics/metrics_collector.h"
#include "metrics/trade_logger.h"

// Feed (Phase 3)
// #include "feed/coinbase_feed.h"
// #include "feed/feed_handler.h"

namespace {

// ---------------------------------------------------------------------------
// Version and banner
// ---------------------------------------------------------------------------

constexpr const char* ENGINE_NAME    = "matching-engine";
constexpr const char* ENGINE_VERSION = "0.1.0";

void print_banner() {
    std::cout << "\n";
    std::cout << "  ╔══════════════════════════════════════════╗\n";
    std::cout << "  ║         LOW-LATENCY MATCHING ENGINE      ║\n";
    std::cout << "  ║              v" << ENGINE_VERSION << "                       ║\n";
    std::cout << "  ╚══════════════════════════════════════════╝\n";
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

/// Global flag for clean shutdown. Set to false by the signal handler.
std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    std::cout << "\n[SIGNAL] Received signal " << signum << ", shutting down...\n";
    g_running.store(false, std::memory_order_release);
}

void install_signal_handlers() {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
}

// ---------------------------------------------------------------------------
// Command-line parsing
// ---------------------------------------------------------------------------

enum class RunMode {
    Synthetic,  ///< Generate synthetic orders for benchmarking
    Live        ///< Connect to live exchange feed (Phase 3)
};

struct Config {
    RunMode mode = RunMode::Synthetic;
};

Config parse_args(int argc, char* argv[]) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};

        if (arg == "--synthetic" || arg == "-s") {
            config.mode = RunMode::Synthetic;
        } else if (arg == "--live" || arg == "-l") {
            config.mode = RunMode::Live;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]\n"
                      << "\n"
                      << "Options:\n"
                      << "  --synthetic, -s    Run with synthetic order data (default)\n"
                      << "  --live, -l         Connect to live exchange feed (Phase 3)\n"
                      << "  --help, -h         Show this help message\n";
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            std::cerr << "Use --help for usage information.\n";
            std::exit(1);
        }
    }

    return config;
}

} // anonymous namespace

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char* argv[]) {
    print_banner();
    install_signal_handlers();

    const auto config = parse_args(argc, argv);

    std::cout << "[INIT] Mode: "
              << (config.mode == RunMode::Synthetic ? "synthetic" : "live")
              << "\n";

    // TODO: Phase 1 — Initialize the matching engine.
    //
    //   engine::MatchingEngine engine;
    //   std::cout << "[INIT] Order pool: " << engine.pool_available()
    //             << " slots available\n";

    // TODO: Phase 1 — Create SPSC queues for inter-thread communication.
    //
    //   using InputQueue  = engine::SPSCQueue<engine::EngineMessage, 65536>;
    //   using OutputQueue = engine::SPSCQueue<engine::OutputMessage, 65536>;
    //   InputQueue  input_queue;
    //   OutputQueue output_queue;

    // TODO: Phase 2 — Initialize metrics and logging.
    //
    //   engine::MetricsCollector metrics;
    //   engine::TradeLogger logger;
    //   logger.open("trades.bin");

    // TODO: Phase 2 — Spawn engine thread.
    //
    //   std::thread engine_thread([&]() {
    //       // Pin to core 1 for consistent latency:
    //       //   SetThreadAffinityMask(GetCurrentThread(), 1 << 1);
    //       //
    //       // Engine loop:
    //       //   while (g_running.load(std::memory_order_acquire)) {
    //       //       engine::EngineMessage msg;
    //       //       if (input_queue.try_pop(msg)) {
    //       //           auto start = now_ns();
    //       //           auto fills = std::visit(overloaded{
    //       //               [&](const engine::NewOrderMessage& m) {
    //       //                   return engine.process_new_order(m);
    //       //               },
    //       //               [&](const engine::CancelMessage& m) -> std::vector<engine::Fill> {
    //       //                   engine.process_cancel(m);
    //       //                   return {};
    //       //               }
    //       //           }, msg);
    //       //           auto elapsed = now_ns() - start;
    //       //           metrics.record_match_latency(elapsed);
    //       //       }
    //       //   }
    //   });

    // TODO: Phase 2 — Spawn feed thread (synthetic or live).
    //
    //   if (config.mode == RunMode::Synthetic) {
    //       // Generate random orders and push into input_queue
    //   } else {
    //       // Phase 3: Start CoinbaseFeed
    //   }

    // TODO: Phase 2 — Spawn output thread.
    //
    //   std::thread output_thread([&]() {
    //       // Pin to core 2
    //       // Drain output_queue, log fills, update metrics
    //   });

    std::cout << "[INIT] Matching engine starting...\n";
    std::cout << "[INIT] Press Ctrl+C to stop.\n";

    // TODO: Replace this with the actual main loop / thread join.
    // For now, just wait for shutdown signal.
    // while (g_running.load(std::memory_order_acquire)) {
    //     std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // }

    std::cout << "[SHUTDOWN] Matching engine stopped cleanly.\n";

    // TODO: Phase 2 — Join threads, flush logs, print final metrics.
    //   engine_thread.join();
    //   output_thread.join();
    //   logger.flush();
    //   logger.close();
    //   std::cout << metrics.summary();

    return 0;
}
