// replay_tool.cpp — Replays a binary order file into the matching engine.
//
// Usage:
//   replay-tool <input_file>
//
// Reads the binary format produced by synthetic_generator:
//   [1 byte tag][payload] per record
//     tag 0x01 = NewOrderMessage
//     tag 0x02 = CancelMessage
//
// TODO: This is a skeleton — the actual engine integration is not yet wired.

#include "core/types.h"
#include "transport/message.h"
// TODO: #include "core/matching_engine.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// Wire‐format tags (must match synthetic_generator.cpp)
// ---------------------------------------------------------------------------
namespace {
constexpr std::uint8_t kTagNewOrder = 0x01;
constexpr std::uint8_t kTagCancel   = 0x02;
} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: replay-tool <input_file>\n";
        return 1;
    }

    const std::string input_path = argv[1];
    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        std::cerr << "ERROR: cannot open input file: " << input_path << "\n";
        return 1;
    }

    // TODO: Construct the MatchingEngine here.
    // engine::MatchingEngine engine;

    std::size_t new_count    = 0;
    std::size_t cancel_count = 0;
    std::size_t error_count  = 0;

    auto t_start = std::chrono::high_resolution_clock::now();

    char tag = 0;
    while (in.get(tag)) {
        switch (static_cast<std::uint8_t>(tag)) {
        case kTagNewOrder: {
            engine::NewOrderMessage msg{};
            in.read(reinterpret_cast<char*>(&msg), sizeof(msg));
            if (!in) { ++error_count; break; }

            // TODO: Convert msg to engine::Order and submit:
            //   auto* order = pool.allocate();
            //   new (order) engine::Order{
            //       .id = msg.id, .side = msg.side, .type = msg.type,
            //       .price = msg.price, .quantity = msg.quantity,
            //       .timestamp = msg.timestamp
            //   };
            //   engine.submit_order(order);
            ++new_count;
            break;
        }
        case kTagCancel: {
            engine::CancelMessage msg{};
            in.read(reinterpret_cast<char*>(&msg), sizeof(msg));
            if (!in) { ++error_count; break; }

            // TODO: Cancel the order in the engine:
            //   engine.cancel_order(msg.id);
            ++cancel_count;
            break;
        }
        default:
            std::cerr << "WARNING: unknown tag 0x"
                      << std::hex << static_cast<int>(tag) << std::dec
                      << " — skipping rest of file.\n";
            ++error_count;
            goto done;  // can't recover without knowing payload size
        }
    }

done:
    auto t_end = std::chrono::high_resolution_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        t_end - t_start).count();

    std::cout << "Replay complete.\n"
              << "  New orders : " << new_count << "\n"
              << "  Cancels    : " << cancel_count << "\n"
              << "  Errors     : " << error_count << "\n"
              << "  Elapsed    : " << elapsed_us << " us\n";

    // TODO: Print engine statistics:
    //   - Total fills / trades generated
    //   - Final book state (best bid / ask)
    //   - Latency percentiles if instrumented

    return 0;
}
