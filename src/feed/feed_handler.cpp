/// @file feed_handler.cpp
/// @brief FeedHandler implementation — Phase 3 (Weeks 5-6).

#include "feed/feed_handler.h"

namespace engine {

std::optional<EngineMessage> FeedHandler::parse(std::string_view raw_json) {
    // TODO: Phase 3 — Parse Coinbase JSON messages into EngineMessages.
    //
    // Coinbase "level2" update format:
    //   {
    //     "type": "l2update",
    //     "product_id": "BTC-USD",
    //     "time": "2024-01-01T00:00:00.000000Z",
    //     "changes": [["buy", "50000.00", "1.5"]]
    //   }
    //
    // Parsing steps:
    //   1. Use a fast JSON parser (simdjson or nlohmann/json).
    //   2. Check the "type" field:
    //      - "l2update": Create NewOrderMessage or CancelMessage based on
    //        whether size is > 0 (new/update) or == 0 (cancel/remove).
    //      - "match": Create a synthetic fill notification.
    //      - "heartbeat", "subscriptions", etc.: Return std::nullopt.
    //   3. Convert price string to ticks using dollars_to_ticks().
    //   4. Convert size string to Quantity.
    //   5. Generate monotonic order IDs for tracking.
    //
    // Error handling:
    //   - Malformed JSON → log warning, return std::nullopt.
    //   - Unknown message type → return std::nullopt.
    //   - Sequence gap detection → log error (TODO: implement recovery).

    (void)raw_json;  // Suppress unused parameter warning
    return std::nullopt;
}

void FeedHandler::set_output_queue(
    SPSCQueue<EngineMessage, FEED_QUEUE_SIZE>& queue) {
    output_queue_ = &queue;
}

} // namespace engine
