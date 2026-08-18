#include "core/order_book.h"

namespace engine {

OrderBook::OrderBook()
    : levels_(std::make_unique<PriceLevel[]>(MAX_PRICE_TICKS)) {
    order_map_.reserve(100'000);
}

bool OrderBook::add_order(Order* order) {
    if (order->price < 0 || order->price >= MAX_PRICE_TICKS) {
        return false;
    }

    auto [it, inserted] = order_map_.emplace(order->id, order);
    if (!inserted) {
        return false;  // duplicate order id
    }

    levels_[static_cast<size_t>(order->price)].push_back(order);

    if (order->side == Side::Bid) {
        ++bid_count_;
        if (bid_count_ == 1 || order->price > best_bid_) {
            best_bid_ = order->price;
        }
    } else {
        ++ask_count_;
        if (ask_count_ == 1 || order->price < best_ask_) {
            best_ask_ = order->price;
        }
    }

    return true;
}

bool OrderBook::cancel_order(OrderId id) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) {
        return false;
    }

    Order* order = it->second;
    Price  price = order->price;
    Side   side  = order->side;

    levels_[static_cast<size_t>(price)].remove(order);
    order_map_.erase(it);

    if (side == Side::Bid) {
        --bid_count_;
    } else {
        --ask_count_;
    }

    // rescan for the new best only if we emptied the best level
    if (levels_[static_cast<size_t>(price)].empty()) {
        if (side == Side::Bid && price == best_bid_) {
            update_best_bid_after_remove(price);
        } else if (side == Side::Ask && price == best_ask_) {
            update_best_ask_after_remove(price);
        }
    }

    return true;
}

Order* OrderBook::get_order(OrderId id) const {
    auto it = order_map_.find(id);
    return (it != order_map_.end()) ? it->second : nullptr;
}

const PriceLevel& OrderBook::get_level(Price price) const {
    return levels_[static_cast<size_t>(price)];
}

PriceLevel& OrderBook::get_level_mut(Price price) {
    return levels_[static_cast<size_t>(price)];
}

void OrderBook::update_best_bid_after_remove(Price removed_price) {
    // side just became empty: reset the sentinel instead of scanning
    // millions of empty levels down to zero
    if (bid_count_ == 0) {
        best_bid_ = 0;
        return;
    }

    // scan downward for the next non-empty level. Bids sit below asks in an
    // uncrossed book, so the first non-empty level found is a bid level.
    // Liquidity clusters near the top of book, making this scan short
    for (Price p = removed_price - 1; p >= 0; --p) {
        if (!levels_[static_cast<size_t>(p)].empty()) {
            best_bid_ = p;
            return;
        }
    }
}

void OrderBook::update_best_ask_after_remove(Price removed_price) {
    if (ask_count_ == 0) {
        best_ask_ = MAX_PRICE_TICKS - 1;
        return;
    }

    for (Price p = removed_price + 1; p < MAX_PRICE_TICKS; ++p) {
        if (!levels_[static_cast<size_t>(p)].empty()) {
            best_ask_ = p;
            return;
        }
    }
}

} // namespace engine
