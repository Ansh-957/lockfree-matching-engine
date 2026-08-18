#pragma once

// Price-time priority matching engine. Owns the order book and the order
// memory pool; single-threaded by design - one engine thread drains an SPSC
// queue and calls process() for each message
//
// Matching algorithm for an incoming order:
//   - buys walk ask levels from best_ask upward, sells walk bid levels from
//     best_bid downward
//   - at each level, fill against resting orders front-to-back (FIFO = time
//     priority; the level walk = price priority)
//   - limit orders stop at their limit price and rest any remainder;
//     market orders keep going until filled or the side is empty, and any
//     remainder is discarded (market orders never rest)
//   - fills execute at the PASSIVE order's price (the resting order set the
//     price; the aggressor accepted it)

#include <vector>

#include "core/types.h"
#include "core/order.h"
#include "core/order_book.h"
#include "core/memory_pool.h"
#include "transport/message.h"

namespace engine {

// a fill IS the outbound fill message - no translation step
using Fill = FillMessage;

class MatchingEngine {
public:
    static constexpr size_t ORDER_POOL_SIZE = 1'000'000;

    MatchingEngine();

    MatchingEngine(const MatchingEngine&)            = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&)                 = delete;
    MatchingEngine& operator=(MatchingEngine&&)      = delete;

    // dispatch on the message variant; returned fills are valid until the
    // next process call (the vector is reused, not reallocated)
    const std::vector<Fill>& process(const EngineMessage& msg);

    // match, then rest any limit remainder. Returns fills generated
    const std::vector<Fill>& process_new_order(const NewOrderMessage& msg);

    // returns false if the order id is unknown (already filled or bogus)
    bool process_cancel(const CancelMessage& msg);

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] size_t pool_available() const noexcept { return pool_.available(); }

private:
    // fill against resting orders at one price level until the incoming
    // order is satisfied or the level is empty; returns updated remaining
    Quantity fill_level(Price level_price, OrderId aggressive_id,
                        Timestamp ts, Quantity remaining);

    OrderBook                          book_;
    MemoryPool<Order, ORDER_POOL_SIZE> pool_;

    // reused across calls so steady-state matching does zero heap allocs;
    // reserved in the constructor so early sweeps don't trigger growth
    std::vector<Fill> fills_;
};

} // namespace engine
