#include "core/matching_engine.h"

#include <algorithm>
#include <new>

namespace engine {

MatchingEngine::MatchingEngine() {
    fills_.reserve(1024);
}

const std::vector<Fill>& MatchingEngine::process(const EngineMessage& msg) {
    if (const auto* order = std::get_if<NewOrderMessage>(&msg)) {
        return process_new_order(*order);
    }
    fills_.clear();
    process_cancel(std::get<CancelMessage>(msg));
    return fills_;
}

const std::vector<Fill>& MatchingEngine::process_new_order(const NewOrderMessage& msg) {
    fills_.clear();

    Quantity remaining = msg.quantity;
    if (remaining == 0) {
        return fills_;
    }

    const bool is_limit = (msg.type == OrderType::Limit);
    if (is_limit && (msg.price < 0 || msg.price >= MAX_PRICE_TICKS)) {
        return fills_;
    }

    // match against the opposite side, best price first. The book repairs
    // best_bid/best_ask as levels empty, so re-reading it each iteration
    // walks the levels in price order
    if (msg.side == Side::Bid) {
        while (remaining > 0 && book_.has_asks()) {
            const Price level_price = book_.best_ask();
            if (is_limit && level_price > msg.price) {
                break;  // book no longer crosses our limit
            }
            remaining = fill_level(level_price, msg.id, msg.timestamp, remaining);
        }
    } else {
        while (remaining > 0 && book_.has_bids()) {
            const Price level_price = book_.best_bid();
            if (is_limit && level_price < msg.price) {
                break;
            }
            remaining = fill_level(level_price, msg.id, msg.timestamp, remaining);
        }
    }

    // rest the remainder - limit orders only, market remainders are discarded.
    // The pool is only touched if something actually rests: pure takers make
    // no allocation at all, and an exhausted pool degrades to dropping the
    // residual instead of crashing
    if (remaining > 0 && is_limit) {
        Order* order = pool_.allocate();
        if (order == nullptr) {
            return fills_;
        }
        new (order) Order{};
        order->id              = msg.id;
        order->side            = msg.side;
        order->type            = msg.type;
        order->price           = msg.price;
        order->quantity        = msg.quantity;
        order->filled_quantity = msg.quantity - remaining;
        order->timestamp       = msg.timestamp;

        if (!book_.add_order(order)) {  // duplicate id
            order->~Order();
            pool_.deallocate(order);
        }
    }

    return fills_;
}

bool MatchingEngine::process_cancel(const CancelMessage& msg) {
    Order* order = book_.get_order(msg.id);
    if (order == nullptr) {
        return false;
    }

    book_.cancel_order(msg.id);

    order->~Order();
    pool_.deallocate(order);
    return true;
}

Quantity MatchingEngine::fill_level(Price level_price, OrderId aggressive_id,
                                    Timestamp ts, Quantity remaining) {
    PriceLevel& level = book_.get_level_mut(level_price);

    while (remaining > 0 && !level.empty()) {
        Order* passive = level.front();

        const Quantity qty = std::min(remaining, passive->remaining_quantity());

        // fill state must be updated BEFORE any removal, so the level's
        // aggregate (which subtracts remaining_quantity on remove) stays
        // consistent - see the ordering rule in price_level.h
        passive->filled_quantity += qty;
        level.reduce_quantity(qty);
        remaining -= qty;

        // execution at the passive order's price
        fills_.push_back({aggressive_id, passive->id, level_price, qty, ts});

        if (passive->is_filled()) {
            // unlinks (subtracting 0 remaining), erases from the id map,
            // fixes side counters and best prices if the level emptied
            book_.cancel_order(passive->id);
            passive->~Order();
            pool_.deallocate(passive);
        }
    }

    return remaining;
}

} // namespace engine
