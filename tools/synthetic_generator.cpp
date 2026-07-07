// synthetic_generator.cpp — Generates synthetic order messages for replay.
//
// Usage:
//   synthetic-generator [options]
//
// Options:
//   --num_orders   N        Number of records to generate    (default: 10000)
//   --cancel_rate  F        Fraction of records that cancel  (default: 0.2)
//   --price_mean   F        Mean price in dollars            (default: 100.0)
//   --price_stddev F        Stddev for price normal dist     (default: 2.0)
//   --output       FILE     Output binary file path          (default: orders.bin)
//
// Binary format (per record):
//   [1 byte tag] [payload]
//     tag 0x01 = NewOrderMessage  (from transport/message.h)
//     tag 0x02 = CancelMessage    (from transport/message.h)
//
// The replay_tool reads this format to feed orders into the engine.

#include "core/types.h"
#include "transport/message.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Wire‐format tags
// ---------------------------------------------------------------------------
namespace {
constexpr std::uint8_t kTagNewOrder = 0x01;
constexpr std::uint8_t kTagCancel   = 0x02;
} // namespace

// ---------------------------------------------------------------------------
// Command-line parsing (simple key-value)
// ---------------------------------------------------------------------------
struct Config {
    std::size_t num_orders   = 10'000;
    double      cancel_rate  = 0.20;
    double      price_mean   = 100.0;
    double      price_stddev = 2.0;
    std::string output_file  = "orders.bin";
};

static Config parse_args(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc - 1; i += 2) {
        std::string key = argv[i];
        std::string val = argv[i + 1];

        if (key == "--num_orders")        cfg.num_orders   = std::stoull(val);
        else if (key == "--cancel_rate")  cfg.cancel_rate  = std::stod(val);
        else if (key == "--price_mean")   cfg.price_mean   = std::stod(val);
        else if (key == "--price_stddev") cfg.price_stddev = std::stod(val);
        else if (key == "--output")       cfg.output_file  = val;
        else {
            std::cerr << "Unknown option: " << key << "\n";
        }
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Timestamp helper
// ---------------------------------------------------------------------------
static engine::Timestamp now_ns() {
    using clock = std::chrono::high_resolution_clock;
    return static_cast<engine::Timestamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now().time_since_epoch())
            .count());
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    Config cfg = parse_args(argc, argv);

    std::ofstream out(cfg.output_file, std::ios::binary);
    if (!out) {
        std::cerr << "ERROR: cannot open output file: " << cfg.output_file << "\n";
        return 1;
    }

    std::mt19937_64 rng(42);  // deterministic seed for reproducibility
    std::normal_distribution<double>  price_dist(cfg.price_mean, cfg.price_stddev);
    std::uniform_real_distribution<double> cancel_coin(0.0, 1.0);
    std::bernoulli_distribution       side_dist(0.5);       // 50 % bid / ask
    std::uniform_int_distribution<int> type_dist(0, 9);     // 10 % market
    std::uniform_int_distribution<engine::Quantity> qty_dist(1, 500);

    std::vector<engine::OrderId> live_order_ids;
    live_order_ids.reserve(cfg.num_orders);

    engine::OrderId next_id = 1;
    std::size_t     new_count = 0;
    std::size_t     cancel_count = 0;

    for (std::size_t i = 0; i < cfg.num_orders; ++i) {

        // Decide: emit a cancel for an existing order, or a new order?
        bool do_cancel = !live_order_ids.empty()
                         && cancel_coin(rng) < cfg.cancel_rate;

        if (do_cancel) {
            // Pick a random live order to cancel
            std::uniform_int_distribution<std::size_t> idx_dist(
                0, live_order_ids.size() - 1);
            std::size_t idx = idx_dist(rng);

            engine::CancelMessage msg{};
            msg.id        = live_order_ids[idx];
            msg.timestamp = now_ns();

            out.put(static_cast<char>(kTagCancel));
            out.write(reinterpret_cast<const char*>(&msg), sizeof(msg));

            // Remove from live set (swap-and-pop)
            std::swap(live_order_ids[idx], live_order_ids.back());
            live_order_ids.pop_back();

            ++cancel_count;
        } else {
            // Generate price in ticks from a normal distribution in dollars
            double price_dollars = std::max(0.01, price_dist(rng));
            engine::Price price_ticks = engine::dollars_to_ticks(price_dollars);

            engine::NewOrderMessage msg{};
            msg.id        = next_id++;
            msg.side      = side_dist(rng) ? engine::Side::Ask : engine::Side::Bid;
            msg.type      = (type_dist(rng) == 0) ? engine::OrderType::Market
                                                   : engine::OrderType::Limit;
            msg.price     = price_ticks;
            msg.quantity  = qty_dist(rng);
            msg.timestamp = now_ns();

            out.put(static_cast<char>(kTagNewOrder));
            out.write(reinterpret_cast<const char*>(&msg), sizeof(msg));

            live_order_ids.push_back(msg.id);
            ++new_count;
        }
    }

    out.flush();
    out.close();

    std::cout << "Wrote " << cfg.num_orders << " records to "
              << cfg.output_file << "\n"
              << "  New orders : " << new_count << "\n"
              << "  Cancels    : " << cancel_count << "\n"
              << "  Live orders remaining: " << live_order_ids.size() << "\n";

    return 0;
}
