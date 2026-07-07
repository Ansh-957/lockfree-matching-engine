/// @file order_book.cpp
/// @brief Implementation of OrderBook methods.

#include "core/order_book.h"

namespace engine {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

OrderBook::OrderBook() {
    // levels_ array is default-constructed (all PriceLevels start empty).
    // Reserve some initial capacity for the order map to reduce rehashing.
    order_map_.reserve(100'000);
}

// ---------------------------------------------------------------------------
// add_order
// ---------------------------------------------------------------------------

bool OrderBook::add_order(Order* order) {
    // TODO: Phase 1 — Full implementation
    //
    // Algorithm:
    //   1. Validate the order's price is in range [0, MAX_PRICE_TICKS).
    //   2. Check that the order ID is not already in order_map_ (reject duplicates).
    //   3. Insert the order into order_map_ for O(1) ID lookup.
    //   4. Push the order onto the back of levels_[order->price].
    //   5. Update best bid/ask tracking:
    //      - If order is a Bid and order->price > best_bid_, update best_bid_.
    //      - If order is an Ask and order->price < best_ask_, update best_ask_.
    //   6. Return true on success.

    if (order->price < 0 || order->price >= MAX_PRICE_TICKS) {
        return false;
    }

    auto [it, inserted] = order_map_.emplace(order->id, order);
    if (!inserted) {
        return false;  // Duplicate order ID
    }

    levels_[static_cast<size_t>(order->price)].push_back(order);

    // Update best price tracking.
    if (order->side == Side::Bid) {
        if (order->price > best_bid_) {
            best_bid_ = order->price;
        }
    } else {
        if (order->price < best_ask_) {
            best_ask_ = order->price;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// cancel_order
// ---------------------------------------------------------------------------

bool OrderBook::cancel_order(OrderId id) {
    // TODO: Phase 1 — Full implementation
    //
    // Algorithm:
    //   1. Look up the order in order_map_. If not found, return false.
    //   2. Remove the order from its PriceLevel (O(1) intrusive list removal).
    //   3. If that was the last order at that price level, update best bid/ask:
    //      - For bids: if removed_price == best_bid_, scan downward to find
    //        the next non-empty bid level. This is O(k) where k is the gap,
    //        but cancel-at-best is the common case (k ≈ 1).
    //      - For asks: symmetric — scan upward.
    //   4. Remove the order from order_map_.
    //   5. Return true. (Caller is responsible for deallocating the Order.)

    auto it = order_map_.find(id);
    if (it == order_map_.end()) {
        return false;
    }

    Order* order = it->second;
    Price price = order->price;
    Side side = order->side;

    levels_[static_cast<size_t>(price)].remove(order);
    order_map_.erase(it);

    // Update best price if we emptied the best level.
    if (levels_[static_cast<size_t>(price)].empty()) {
        if (side == Side::Bid && price == best_bid_) {
            update_best_bid_after_remove(price);
        } else if (side == Side::Ask && price == best_ask_) {
            update_best_ask_after_remove(price);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// get_order
// ---------------------------------------------------------------------------

Order* OrderBook::get_order(OrderId id) const {
    auto it = order_map_.find(id);
    return (it != order_map_.end()) ? it->second : nullptr;
}

// ---------------------------------------------------------------------------
// get_level
// ---------------------------------------------------------------------------

const PriceLevel& OrderBook::get_level(Price price) const {
    return levels_[static_cast<size_t>(price)];
}

PriceLevel& OrderBook::get_level_mut(Price price) {
    return levels_[static_cast<size_t>(price)];
}

// ---------------------------------------------------------------------------
// has_bids / has_asks
// ---------------------------------------------------------------------------

bool OrderBook::has_bids() const noexcept {
    // If best_bid_ is 0 and that level is empty, there are no bids.
    return !levels_[static_cast<size_t>(best_bid_)].empty();
}

bool OrderBook::has_asks() const noexcept {
    return !levels_[static_cast<size_t>(best_ask_)].empty();
}

// ---------------------------------------------------------------------------
// Best price maintenance helpers
// ---------------------------------------------------------------------------

void OrderBook::update_best_bid_after_remove(Price removed_price) {
    // Scan downward from the removed price to find the next non-empty bid level.
    // In practice, the book is dense near the top of book, so this is fast.
    for (Price p = removed_price - 1; p >= 0; --p) {
        if (!levels_[static_cast<size_t>(p)].empty()) {
            best_bid_ = p;
            return;
        }
    }
    // No bids remain.
    best_bid_ = 0;
}

void OrderBook::update_best_ask_after_remove(Price removed_price) {
    // Scan upward from the removed price to find the next non-empty ask level.
    for (Price p = removed_price + 1; p < MAX_PRICE_TICKS; ++p) {
        if (!levels_[static_cast<size_t>(p)].empty()) {
            best_ask_ = p;
            return;
        }
    }
    // No asks remain.
    best_ask_ = MAX_PRICE_TICKS - 1;
}

} // namespace engine
