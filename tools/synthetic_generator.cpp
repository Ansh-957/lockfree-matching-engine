// CLI wrapper around synth::WorkloadGenerator - generates a synthetic
// order stream and writes it to a binary file for the replay tool
//
// Usage:
//   synthetic-generator [options]
//
// Options:
//   --num_orders   N     Number of records to generate       (default: 10000)
//   --cancel_rate  F     Fraction of records that cancel     (default: 0.2)
//   --market_rate  F     Fraction of new orders = market     (default: 0.1)
//   --price_mean   F     Mean price in dollars               (default: 100.0)
//   --price_stddev F     Stddev for price normal dist        (default: 2.0)
//   --seed         N     RNG seed                            (default: 42)
//   --output       FILE  Output binary file path             (default: orders.bin)
//
// Binary format (per record):
//   [1 byte tag] [raw struct bytes]
//     tag 0x01 = NewOrderMessage
//     tag 0x02 = CancelMessage

#include "synthetic_workload.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <variant>

namespace {

constexpr std::uint8_t kTagNewOrder = 0x01;
constexpr std::uint8_t kTagCancel   = 0x02;

struct Config {
    std::size_t num_orders = 10'000;
    engine::synth::WorkloadConfig workload{};
    std::string output_file = "orders.bin";
};

Config parse_args(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc - 1; i += 2) {
        const std::string key = argv[i];
        const std::string val = argv[i + 1];

        if (key == "--num_orders")        cfg.num_orders                    = std::stoull(val);
        else if (key == "--cancel_rate")  cfg.workload.cancel_rate          = std::stod(val);
        else if (key == "--market_rate")  cfg.workload.market_rate          = std::stod(val);
        else if (key == "--price_mean")   cfg.workload.price_mean_dollars   = std::stod(val);
        else if (key == "--price_stddev") cfg.workload.price_stddev_dollars = std::stod(val);
        else if (key == "--seed")         cfg.workload.seed                 = std::stoull(val);
        else if (key == "--output")       cfg.output_file                   = val;
        else {
            std::cerr << "Unknown option: " << key << "\n";
        }
    }
    return cfg;
}

} // namespace

int main(int argc, char* argv[]) {
    const Config cfg = parse_args(argc, argv);

    std::ofstream out(cfg.output_file, std::ios::binary);
    if (!out) {
        std::cerr << "ERROR: cannot open output file: " << cfg.output_file << "\n";
        return 1;
    }

    engine::synth::WorkloadGenerator gen(cfg.workload);

    std::size_t new_count = 0;
    std::size_t cancel_count = 0;

    for (std::size_t i = 0; i < cfg.num_orders; ++i) {
        const engine::EngineMessage msg = gen.next();

        if (const auto* order = std::get_if<engine::NewOrderMessage>(&msg)) {
            out.put(static_cast<char>(kTagNewOrder));
            out.write(reinterpret_cast<const char*>(order), sizeof(*order));
            ++new_count;
        } else {
            const auto& cancel = std::get<engine::CancelMessage>(msg);
            out.put(static_cast<char>(kTagCancel));
            out.write(reinterpret_cast<const char*>(&cancel), sizeof(cancel));
            ++cancel_count;
        }
    }

    out.flush();
    if (!out) {
        std::cerr << "ERROR: write failed for: " << cfg.output_file << "\n";
        return 1;
    }

    std::cout << "Wrote " << cfg.num_orders << " records to " << cfg.output_file << "\n"
              << "  New orders : " << new_count << "\n"
              << "  Cancels    : " << cancel_count << "\n"
              << "  Live orders remaining: " << gen.live_order_count() << "\n";

    return 0;
}
