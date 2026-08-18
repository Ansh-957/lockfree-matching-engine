#pragma once

// Order with embedded intrusive list pointers. The order IS the list node:
// no separate node allocation, O(1) unlink given a pointer, and no pointer
// chasing to a wrapper struct. Deliberately not alignas(64) - keeping the
// struct compact fits more orders per cache line

#include "core/types.h"

namespace engine {

struct Order {
    OrderId   id              = 0;
    Side      side            = Side::Bid;
    OrderType type            = OrderType::Limit;
    Price     price           = 0;                 // ticks
    Quantity  quantity        = 0;                 // original size
    Quantity  filled_quantity = 0;                 // cumulative fills
    Timestamp timestamp       = 0;

    // managed by PriceLevel; nullptr when the order is not in any queue
    Order* prev = nullptr;
    Order* next = nullptr;

    [[nodiscard]] Quantity remaining_quantity() const noexcept {
        return quantity - filled_quantity;
    }

    [[nodiscard]] bool is_filled() const noexcept {
        return filled_quantity >= quantity;
    }
};

} // namespace engine
