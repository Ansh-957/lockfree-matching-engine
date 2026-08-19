#pragma once

// Coinbase Exchange JSON → EngineMessage. No sockets, no Boost: this is
// a pure parser so the unit tests can run without TLS or a network.
//
// Coinbase L2 is *aggregated depth* (quantity at a price), not individual
// orders. We synthesize one resting limit order per (side, price) and
// cancel-replace it whenever the level's quantity changes. A size of 0
// removes the level. Snapshots reset the whole synthetic book first.
//
// One WebSocket frame can produce many messages (a snapshot is thousands
// of levels), so parse() appends into a caller-provided vector rather
// than returning optional<EngineMessage>.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "core/types.h"
#include "transport/message.h"

namespace engine {

// feed → engine ring size. Power of two for the SPSC bitmask. 64K slots
// of ~48-byte messages is ~3MB, enough to absorb a snapshot burst.
inline constexpr std::size_t FEED_QUEUE_SIZE = 65536;

// 1 quantity unit = 1e-6 coin. Coinbase sizes are fractional BTC;
// Quantity is an integer. 1.5 BTC → 1,500,000. uint32 max ≈ 4294 coins
// at this scale, which covers any single L2 level we will see.
inline constexpr double LOTS_PER_COIN = 1'000'000.0;

class FeedHandler {
public:
    FeedHandler();
    ~FeedHandler();

    FeedHandler(const FeedHandler&)            = delete;
    FeedHandler& operator=(const FeedHandler&) = delete;
    FeedHandler(FeedHandler&&)                 = delete;
    FeedHandler& operator=(FeedHandler&&)      = delete;

    // Append 0 or more messages produced by this frame. Returns how many
    // were appended. Malformed JSON / unknown types append nothing.
    // The string_view need only live for this call (we copy into a padded
    // buffer - simdjson requires 64 bytes of padding past the JSON).
    std::size_t parse(std::string_view raw_json, std::vector<EngineMessage>& out);

    [[nodiscard]] uint64_t malformed_count() const noexcept;
    [[nodiscard]] uint64_t sequence_gaps()   const noexcept;
    [[nodiscard]] uint64_t ignored_count()   const noexcept;
    [[nodiscard]] uint64_t skipped_count()   const noexcept;  // valid JSON, price outside the book
    [[nodiscard]] std::size_t live_levels()  const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace engine
