#pragma once

// Central limit order book with O(1) price level access
//
// A single flat array of PriceLevels indexed directly by price in ticks:
// levels_[p] is the order queue at p ticks. ~240MB allocated once at
// startup buys O(1) lookup at any price with zero pointer chasing
//
// Bids and asks share the array. This relies on the book staying uncrossed
// (best bid < best ask), which the matching engine guarantees by consuming
// crossing volume before resting an order - so any given level only ever
// holds one side. Per-side order counts make has_bids/has_asks exact and
// let cancel skip the best-price rescan when a side just became empty
//
// Best bid/ask are updated lazily: adds compare against the current best;
// a cancel that empties the best level scans inward for the next non-empty
// level. Cancels cluster near the top of book, so the scan is short in
// practice

#include <memory>
#include <unordered_map>
#include <cstdint>

#include "core/types.h"
#include "core/order.h"
#include "core/price_level.h"

namespace engine {

class OrderBook {
public:
    OrderBook();
    ~OrderBook() = default;

    // owns a huge buffer and hands out pointers into it
    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&)                 = delete;
    OrderBook& operator=(OrderBook&&)      = delete;

    // rest a fully constructed order at its price level
    // returns false on duplicate id or out-of-range price
    bool add_order(Order* order);

    // remove an order by id; returns false if not found
    // the caller still owns the Order memory and must deallocate it
    bool cancel_order(OrderId id);

    // nullptr if not found
    [[nodiscard]] Order* get_order(OrderId id) const;

    // valid for 0 <= price < MAX_PRICE_TICKS
    [[nodiscard]] const PriceLevel& get_level(Price price) const;
    [[nodiscard]] PriceLevel& get_level_mut(Price price);

    // sentinels when the side is empty (0 / MAX_PRICE_TICKS - 1);
    // check has_bids()/has_asks() before trusting these
    [[nodiscard]] Price best_bid() const noexcept { return best_bid_; }
    [[nodiscard]] Price best_ask() const noexcept { return best_ask_; }

    // meaningless if either side is empty
    [[nodiscard]] Price spread() const noexcept { return best_ask_ - best_bid_; }

    [[nodiscard]] bool has_bids() const noexcept { return bid_count_ > 0; }
    [[nodiscard]] bool has_asks() const noexcept { return ask_count_ > 0; }

    // resting order counts per side
    [[nodiscard]] uint64_t bid_count() const noexcept { return bid_count_; }
    [[nodiscard]] uint64_t ask_count() const noexcept { return ask_count_; }

private:
    void update_best_bid_after_remove(Price removed_price);
    void update_best_ask_after_remove(Price removed_price);

    // heap-backed: 10M levels * 24 bytes would make sizeof(OrderBook) ~240MB
    // if the array were an inline member, blowing the stack the moment
    // anyone declares a book (or a MatchingEngine holding one) as a local
    std::unique_ptr<PriceLevel[]> levels_;

    Price best_bid_ = 0;
    Price best_ask_ = MAX_PRICE_TICKS - 1;

    uint64_t bid_count_ = 0;
    uint64_t ask_count_ = 0;

    // OrderId -> Order* for O(1) cancels; pre-sized to limit rehashing
    // future work: dense slot array with generation counters to avoid
    // hash map cache misses
    std::unordered_map<OrderId, Order*> order_map_;
};

} // namespace engine
