#pragma once

/// @file order.h
/// @brief Order structure with intrusive doubly-linked list pointers.
///
/// Each Order embeds prev/next pointers for the intrusive list used by PriceLevel.
/// This design avoids separate node allocations (as std::list would require),
/// keeping all order data in a single contiguous memory pool. When an order is
/// inserted into a PriceLevel's queue, no additional heap allocation occurs —
/// we simply wire up the embedded pointers.
///
/// We intentionally do NOT use alignas(64) on Order itself to keep the struct
/// compact (~56 bytes). Cache-line alignment is handled at the MemoryPool level
/// if needed for specific access patterns.

#include "core/types.h"

namespace engine {

/// @brief Represents a single order in the book.
///
/// Orders live in a MemoryPool and are linked into PriceLevel queues via
/// the embedded prev/next pointers (intrusive doubly-linked list pattern).
struct Order {
    OrderId   id              = 0;                 ///< Unique order identifier
    Side      side            = Side::Bid;         ///< Buy or sell
    OrderType type            = OrderType::Limit;  ///< Limit or market
    Price     price           = 0;                 ///< Price in ticks
    Quantity  quantity         = 0;                 ///< Original quantity
    Quantity  filled_quantity  = 0;                 ///< Cumulative filled quantity
    Timestamp timestamp       = 0;                 ///< Order creation time (ns since epoch)

    // -----------------------------------------------------------------------
    // Intrusive list pointers
    // -----------------------------------------------------------------------
    // These are managed by PriceLevel. When the order is in a price level's
    // FIFO queue, prev/next point to adjacent orders at the same price.
    // When the order is not in any queue, both should be nullptr.
    //
    // Why intrusive? Because:
    //   1. No separate node allocation — the order IS the node.
    //   2. O(1) removal given a pointer to the order (no search needed).
    //   3. Better cache locality — no pointer chasing to a separate node struct.
    // -----------------------------------------------------------------------
    Order* prev = nullptr;  ///< Previous order at same price level (or nullptr)
    Order* next = nullptr;  ///< Next order at same price level (or nullptr)

    // -----------------------------------------------------------------------
    // Convenience accessors
    // -----------------------------------------------------------------------

    /// Remaining quantity that has not yet been filled.
    [[nodiscard]] Quantity remaining_quantity() const noexcept {
        return quantity - filled_quantity;
    }

    /// Whether this order has been fully filled.
    [[nodiscard]] bool is_filled() const noexcept {
        return filled_quantity >= quantity;
    }
};

} // namespace engine
