/// @file matching_engine.cpp
/// @brief Implementation of the matching engine's order processing logic.

#include "core/matching_engine.h"

#include <new>  // for placement new

namespace engine {

// ---------------------------------------------------------------------------
// process_new_order
// ---------------------------------------------------------------------------

std::vector<Fill> MatchingEngine::process_new_order(const NewOrderMessage& msg) {
    // TODO: Phase 1 — Implement the matching algorithm.
    //
    // Detailed algorithm (price-time priority matching):
    //
    //   1. ALLOCATE: Get raw memory from pool_ and placement-new an Order.
    //
    //      Order* order = pool_.allocate();
    //      new (order) Order{
    //          .id = msg.id, .side = msg.side, .type = msg.type,
    //          .price = msg.price, .quantity = msg.quantity,
    //          .filled_quantity = 0, .timestamp = msg.timestamp
    //      };
    //
    //   2. MATCH: Walk the opposite side of the book from the best price.
    //
    //      For a BUY order:
    //        - Start at best_ask (the lowest ask price).
    //        - While order has remaining quantity AND best_ask <= order->price
    //          (or unconditionally for Market orders):
    //            a. Get the PriceLevel at best_ask.
    //            b. While the level is non-empty AND order has remaining qty:
    //               - Take the front() resting order.
    //               - fill_qty = min(order->remaining(), passive->remaining()).
    //               - Update both orders' filled_quantity.
    //               - Update the level's total_quantity (reduce_quantity).
    //               - Emit a Fill{aggressive=order->id, passive=passive->id, ...}.
    //               - If passive is fully filled, pop_front() and deallocate.
    //            c. Move to next ask level (best_ask + 1).
    //
    //      For a SELL order:
    //        - Start at best_bid (the highest bid price).
    //        - Walk downward symmetrically.
    //
    //   3. REST OR DISCARD:
    //      - If order has remaining quantity AND is a Limit order:
    //        - Call book_.add_order(order) to rest it in the book.
    //      - Else (fully filled, or Market order with no more liquidity):
    //        - Destroy and deallocate: order->~Order(); pool_.deallocate(order);
    //
    //   4. RETURN: Return the vector of Fill structs.

    std::vector<Fill> fills;

    // --- Stub: allocate and immediately rest (no matching yet) ---
    Order* order = pool_.allocate();
    new (order) Order{};
    order->id              = msg.id;
    order->side            = msg.side;
    order->type            = msg.type;
    order->price           = msg.price;
    order->quantity        = msg.quantity;
    order->filled_quantity = 0;
    order->timestamp       = msg.timestamp;

    // TODO: Implement matching loop here.
    // For now, just rest the order in the book.
    if (order->type == OrderType::Limit) {
        book_.add_order(order);
    } else {
        // Market orders with no matching logic yet — just discard.
        order->~Order();
        pool_.deallocate(order);
    }

    return fills;
}

// ---------------------------------------------------------------------------
// process_cancel
// ---------------------------------------------------------------------------

bool MatchingEngine::process_cancel(const CancelMessage& msg) {
    // TODO: Phase 1 — Implement cancel logic.
    //
    // Algorithm:
    //   1. Look up the order: Order* order = book_.get_order(msg.id);
    //   2. If not found, return false.
    //   3. Remove from book: book_.cancel_order(msg.id);
    //   4. Destroy and return to pool:
    //        order->~Order();
    //        pool_.deallocate(order);
    //   5. Return true.

    Order* order = book_.get_order(msg.id);
    if (!order) {
        return false;
    }

    book_.cancel_order(msg.id);

    // Return the memory to the pool.
    order->~Order();
    pool_.deallocate(order);

    return true;
}

} // namespace engine
