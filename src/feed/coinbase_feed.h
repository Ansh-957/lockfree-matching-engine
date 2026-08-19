#pragma once

// Live Coinbase Exchange adapter: owns the WebSocket client and the
// parser, and is the SPSC producer. start() blocks the calling thread
// (the ingest thread in Commit 10) until stop() is called from another
// thread.

#include <atomic>
#include <string>
#include <vector>

#include "feed/feed_handler.h"
#include "feed/ws_client.h"
#include "transport/message.h"
#include "transport/spsc_queue.h"

namespace engine {

class CoinbaseFeed {
public:
    explicit CoinbaseFeed(std::string product_id = "BTC-USD");

    CoinbaseFeed(const CoinbaseFeed&)            = delete;
    CoinbaseFeed& operator=(const CoinbaseFeed&) = delete;
    CoinbaseFeed(CoinbaseFeed&&)                 = delete;
    CoinbaseFeed& operator=(CoinbaseFeed&&)      = delete;

    // Connect, subscribe to level2 + matches, parse every frame into
    // EngineMessages and try_push them. Blocks until stop().
    void start(SPSCQueue<EngineMessage, FEED_QUEUE_SIZE>& queue);

    // Thread-safe: posts a close onto the io thread.
    void stop();

    [[nodiscard]] const FeedHandler& handler() const noexcept { return handler_; }
    [[nodiscard]] uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t pushed()  const noexcept { return pushed_.load(std::memory_order_relaxed); }

private:
    std::string product_id_;
    FeedHandler handler_;
    WebSocketClient ws_;
    std::vector<EngineMessage> batch_;
    SPSCQueue<EngineMessage, FEED_QUEUE_SIZE>* queue_ = nullptr;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> pushed_{0};
};

} // namespace engine
