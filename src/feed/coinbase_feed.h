#pragma once

/// @file coinbase_feed.h
/// @brief Coinbase Advanced Trade WebSocket feed adapter — Phase 3.
///
/// Connects to the Coinbase WebSocket feed, subscribes to market data
/// channels, and pushes parsed messages into the engine's SPSC queue.

#include <atomic>
#include <cstddef>
#include <string>

#include "core/types.h"
#include "transport/spsc_queue.h"
#include "transport/message.h"
#include "feed/ws_client.h"

namespace engine {

/// Default SPSC queue size for feed → engine communication.
inline constexpr size_t FEED_QUEUE_SIZE = 65536;  // 2^16, power of 2

/// @brief Adapter that connects to the Coinbase WebSocket feed and produces
///        EngineMessage events for the matching engine.
class CoinbaseFeed {
public:
    CoinbaseFeed() = default;
    ~CoinbaseFeed() = default;

    // Non-copyable, non-movable.
    CoinbaseFeed(const CoinbaseFeed&)            = delete;
    CoinbaseFeed& operator=(const CoinbaseFeed&) = delete;
    CoinbaseFeed(CoinbaseFeed&&)                 = delete;
    CoinbaseFeed& operator=(CoinbaseFeed&&)      = delete;

    /// @brief Start the feed, connecting to Coinbase and pushing messages
    ///        into the provided SPSC queue.
    /// @param queue  The engine's input queue. This call may block (runs the WS event loop).
    void start(SPSCQueue<EngineMessage, FEED_QUEUE_SIZE>& queue);

    /// @brief Signal the feed to stop and disconnect.
    void stop();

private:
    WebSocketClient     ws_client_;                ///< WebSocket connection
    std::atomic<bool>   running_{false};           ///< Shutdown flag
    std::string         product_id_ = "BTC-USD";   ///< Default trading pair
};

} // namespace engine
