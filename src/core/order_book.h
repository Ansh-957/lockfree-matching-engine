#pragma once

/// @file order_book.h
/// @brief Central limit order book with O(1) price level access.
///
/// The order book uses a flat array of PriceLevel objects indexed directly
/// by price (in ticks). This gives O(1) lookup for any price level at the
/// cost of a large up-front allocation (~240 MB for 10M levels × 24 bytes).
///
/// Best bid/ask tracking:
///   - best_bid_ is the highest price with resting bid orders.
///   - best_ask_ is the lowest price with resting ask orders.
///   - These are updated lazily: on add, we only compare with the current best;
///     on cancel/fill that empties a level, we scan to find the new best.
///
/// Order lookup by ID is done via an unordered_map for O(1) amortized cancels.

#include <array>
#include <unordered_map>
#include <cstdint>

#include "core/types.h"
#include "core/order.h"
#include "core/price_level.h"

namespace engine {

/// @brief A central limit order book supporting one symbol/instrument.
///
/// The book maintains a flat array of PriceLevels (one per tick) and tracks
/// best bid/ask prices for efficient matching. Orders are stored in a
/// separate MemoryPool and linked into the appropriate PriceLevel.
class OrderBook {
public:
    OrderBook();
    ~OrderBook() = default;

    // Non-copyable, non-movable — contains a massive levels_ array.
    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&)                 = delete;
    OrderBook& operator=(OrderBook&&)      = delete;

    // -------------------------------------------------------------------
    // Order management
    // -------------------------------------------------------------------

    /// @brief Add a resting order to the book at its price level.
    /// @param order  Pointer to a fully constructed order. Must not already
    ///               be in the book.
    /// @return true on success, false if the order ID is a duplicate or
    ///         the price is out of range.
    bool add_order(Order* order);

    /// @brief Cancel (remove) an order by its ID.
    /// @param id  The order ID to cancel.
    /// @return true if the order was found and removed, false if not found.
    bool cancel_order(OrderId id);

    /// @brief Look up an order by ID.
    /// @return Pointer to the order, or nullptr if not found.
    [[nodiscard]] Order* get_order(OrderId id) const;

    // -------------------------------------------------------------------
    // Price level access
    // -------------------------------------------------------------------

    /// @brief Get the price level at a specific tick price.
    /// @pre 0 <= price < MAX_PRICE_TICKS
    [[nodiscard]] const PriceLevel& get_level(Price price) const;

    /// @brief Mutable access to a price level (used by matching engine).
    /// @pre 0 <= price < MAX_PRICE_TICKS
    [[nodiscard]] PriceLevel& get_level_mut(Price price);

    // -------------------------------------------------------------------
    // Best price tracking
    // -------------------------------------------------------------------

    /// @brief Current best (highest) bid price, or 0 if no bids.
    [[nodiscard]] Price best_bid() const noexcept { return best_bid_; }

    /// @brief Current best (lowest) ask price, or MAX_PRICE_TICKS - 1 if no asks.
    [[nodiscard]] Price best_ask() const noexcept { return best_ask_; }

    /// @brief Spread in ticks between best ask and best bid.
    /// @note Returns a potentially meaningless value if one side is empty.
    [[nodiscard]] Price spread() const noexcept { return best_ask_ - best_bid_; }

    /// @brief Whether there are any bid orders in the book.
    [[nodiscard]] bool has_bids() const noexcept;

    /// @brief Whether there are any ask orders in the book.
    [[nodiscard]] bool has_asks() const noexcept;

private:
    // -------------------------------------------------------------------
    // Best price maintenance helpers
    // -------------------------------------------------------------------
    void update_best_bid_after_remove(Price removed_price);
    void update_best_ask_after_remove(Price removed_price);

    // -------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------

    /// Flat array of price levels, indexed by tick price.
    /// levels_[p] is the queue of orders at price p ticks.
    /// This is the "big array" approach: ~240 MB for 10M levels.
    std::array<PriceLevel, MAX_PRICE_TICKS> levels_;

    /// Best (highest) bid price. Initialized to 0 (no bids).
    Price best_bid_ = 0;

    /// Best (lowest) ask price. Initialized to MAX_PRICE_TICKS - 1 (no asks).
    Price best_ask_ = MAX_PRICE_TICKS - 1;

    /// Maps OrderId → Order* for O(1) cancel by ID.
    std::unordered_map<OrderId, Order*> order_map_;
};

} // namespace engine
