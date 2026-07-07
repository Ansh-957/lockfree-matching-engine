#pragma once

/// @file feed_handler.h
/// @brief Parses raw JSON messages from exchange feeds into EngineMessages.
///
/// The FeedHandler is a stateless parser that converts raw WebSocket text
/// frames into typed EngineMessage variants. It decouples the transport
/// layer (WebSocketClient) from the engine's message format.

#include <optional>
#include <string_view>

#include "core/types.h"
#include "transport/spsc_queue.h"
#include "transport/message.h"
#include "feed/coinbase_feed.h"  // for FEED_QUEUE_SIZE

namespace engine {

/// @brief Parses raw JSON from exchange feeds into EngineMessage variants.
class FeedHandler {
public:
    FeedHandler() = default;
    ~FeedHandler() = default;

    /// @brief Parse a raw JSON message into an EngineMessage.
    /// @param raw_json  The raw JSON string from the WebSocket feed.
    /// @return An EngineMessage if the JSON was successfully parsed and
    ///         represents a relevant message, or std::nullopt if the
    ///         message should be ignored (heartbeats, errors, etc.).
    [[nodiscard]] std::optional<EngineMessage> parse(std::string_view raw_json);

    /// @brief Set the output queue for parsed messages.
    /// @param queue  The engine's input SPSC queue.
    void set_output_queue(SPSCQueue<EngineMessage, FEED_QUEUE_SIZE>& queue);

private:
    SPSCQueue<EngineMessage, FEED_QUEUE_SIZE>* output_queue_ = nullptr;

    // TODO: Phase 3 — Add JSON parser state, sequence tracking, etc.
    //   uint64_t next_order_id_ = 1;  // For generating synthetic order IDs
    //   uint64_t sequence_ = 0;       // For gap detection
};

} // namespace engine
