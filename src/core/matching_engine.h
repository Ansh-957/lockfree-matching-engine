#pragma once

/// @file matching_engine.h
/// @brief The core matching engine: processes orders and generates fills.
///
/// The matching engine owns an OrderBook and a MemoryPool for order storage.
/// It implements price-time priority matching: incoming orders walk the
/// opposite side of the book from the best price, filling against resting
/// orders in FIFO order at each price level.

#include <vector>

#include "core/types.h"
#include "core/order.h"
#include "core/order_book.h"
#include "core/memory_pool.h"
#include "transport/message.h"

namespace engine {

// ---------------------------------------------------------------------------
// Fill struct
// ---------------------------------------------------------------------------

/// @brief Represents a single fill (trade) generated during matching.
///
/// Each fill pairs an aggressive (incoming) order with a passive (resting)
/// order. Multiple fills can result from a single incoming order.
struct Fill {
    OrderId   aggressive_id = 0;  ///< The incoming (taker) order
    OrderId   passive_id    = 0;  ///< The resting (maker) order
    Price     price         = 0;  ///< Execution price in ticks
    Quantity  quantity      = 0;  ///< Fill quantity
    Timestamp timestamp     = 0;  ///< Fill timestamp (ns since epoch)
};

// ---------------------------------------------------------------------------
// MatchingEngine
// ---------------------------------------------------------------------------

/// @brief The core engine: receives order messages, matches them, emits fills.
///
/// Thread safety: NOT thread-safe. Designed to be called from a single thread
/// (the engine thread) that drains messages from an SPSC queue.
///
/// Memory management:
///   - Orders are allocated from the internal MemoryPool (no heap allocs).
///   - Filled orders are automatically deallocated.
///   - Cancelled orders are deallocated after removal from the book.
class MatchingEngine {
public:
    /// Pool size: 1 million pre-allocated order slots.
    static constexpr size_t ORDER_POOL_SIZE = 1'000'000;

    MatchingEngine() = default;
    ~MatchingEngine() = default;

    // Non-copyable, non-movable.
    MatchingEngine(const MatchingEngine&)            = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&)                 = delete;
    MatchingEngine& operator=(MatchingEngine&&)      = delete;

    // -------------------------------------------------------------------
    // Message processing
    // -------------------------------------------------------------------

    /// @brief Process a new order: match against resting orders, then rest remainder.
    /// @param msg  The new order message from the input queue.
    /// @return Vector of fills generated during matching (may be empty).
    ///
    /// Algorithm (price-time priority):
    ///   1. Allocate an Order from the memory pool.
    ///   2. If it's a buy, walk ask levels from best_ask upward.
    ///      If it's a sell, walk bid levels from best_bid downward.
    ///   3. At each level, fill against resting orders in FIFO order.
    ///   4. Generate a Fill for each match.
    ///   5. If the incoming order is not fully filled, rest it in the book.
    ///   6. If it IS fully filled (or is a market order), deallocate it.
    std::vector<Fill> process_new_order(const NewOrderMessage& msg);

    /// @brief Process a cancel request: remove the order from the book.
    /// @param msg  The cancel message from the input queue.
    /// @return true if the order was found and cancelled, false otherwise.
    bool process_cancel(const CancelMessage& msg);

    // -------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------

    /// @brief Read-only access to the order book (for diagnostics/display).
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }

    /// @brief Number of available slots in the order pool.
    [[nodiscard]] size_t pool_available() const noexcept { return pool_.available(); }

private:
    OrderBook                            book_;   ///< The limit order book
    MemoryPool<Order, ORDER_POOL_SIZE>   pool_;   ///< Pre-allocated order storage
};

} // namespace engine
