/// @file coinbase_feed.cpp
/// @brief Coinbase feed implementation — Phase 3 (Weeks 5-6).

#include "feed/coinbase_feed.h"

namespace engine {

void CoinbaseFeed::start(SPSCQueue<EngineMessage, FEED_QUEUE_SIZE>& queue) {
    // TODO: Phase 3 — Implement Coinbase feed connection and message parsing.
    //
    // Steps:
    //   1. Set running_ = true.
    //   2. Configure ws_client_ to connect to "wss://ws-feed.exchange.coinbase.com".
    //   3. Subscribe to the "level2" and "matches" channels for product_id_.
    //      Subscription message format (JSON):
    //        {
    //          "type": "subscribe",
    //          "product_ids": ["BTC-USD"],
    //          "channels": ["level2", "matches"]
    //        }
    //   4. Set message handler to parse incoming JSON and push EngineMessages
    //      into the queue:
    //        ws_client_.set_message_handler([&](std::string_view msg) {
    //          // Parse JSON, create NewOrderMessage or CancelMessage
    //          // queue.try_push(engine_msg);
    //        });
    //   5. ws_client_.run();  // Blocks until disconnect.
    (void)queue;  // Suppress unused parameter warning
}

void CoinbaseFeed::stop() {
    // TODO: Phase 3 — Signal shutdown and disconnect.
    //
    // Steps:
    //   1. running_.store(false);
    //   2. ws_client_.disconnect();
    running_.store(false);
}

} // namespace engine
