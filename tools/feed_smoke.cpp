// Live parser smoke: connect to Coinbase, subscribe to level2+matches,
// drain parsed EngineMessages from the SPSC queue, then stop.
//
// Usage: feed-smoke [num_messages]   (default 20)

#include "feed/coinbase_feed.h"
#include "transport/message.h"
#include "transport/spsc_queue.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <variant>

int main(int argc, char* argv[]) {
    const int max_messages = (argc > 1) ? std::atoi(argv[1]) : 20;

    engine::SPSCQueue<engine::EngineMessage, engine::FEED_QUEUE_SIZE> queue;
    engine::CoinbaseFeed feed{"BTC-USD"};

    std::thread ingest([&] { feed.start(queue); });

    int n = 0;
    int news = 0;
    int cancels = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    engine::EngineMessage msg;
    while (n < max_messages && std::chrono::steady_clock::now() < deadline) {
        if (!queue.try_pop(msg)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        ++n;
        if (std::holds_alternative<engine::NewOrderMessage>(msg)) {
            const auto& o = std::get<engine::NewOrderMessage>(msg);
            ++news;
            if (n <= 5) {
                std::cout << "[new] id=" << o.id
                          << " side=" << (o.side == engine::Side::Bid ? "bid" : "ask")
                          << " px=" << engine::ticks_to_dollars(o.price)
                          << " qty=" << o.quantity << "\n";
            }
        } else {
            ++cancels;
            if (n <= 5) {
                std::cout << "[cancel] id=" << std::get<engine::CancelMessage>(msg).id << "\n";
            }
        }
    }

    feed.stop();
    ingest.join();

    std::cout << "received " << n << " engine messages"
              << " (new=" << news << " cancel=" << cancels << ")\n"
              << "  live levels : " << feed.handler().live_levels() << "\n"
              << "  malformed   : " << feed.handler().malformed_count() << "\n"
              << "  skipped     : " << feed.handler().skipped_count() << "\n"
              << "  seq gaps    : " << feed.handler().sequence_gaps() << "\n"
              << "  ignored     : " << feed.handler().ignored_count() << "\n"
              << "  dropped     : " << feed.dropped() << "\n";

    return n > 0 ? 0 : 1;
}
