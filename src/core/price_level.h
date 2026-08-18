#pragma once

// FIFO queue of orders resting at a single price, implemented as an
// intrusive doubly-linked list through Order::prev / Order::next.
// No allocation happens here - orders live in the memory pool and this
// class only wires their embedded pointers
//
// Invariants:
//   - if non-empty: head_->prev == nullptr and tail_->next == nullptr
//   - total_quantity_ == sum of remaining_quantity() over all linked orders
//   - order_count_ == number of linked orders

#include "core/order.h"

namespace engine {

class PriceLevel {
public:
    PriceLevel() = default;

    // these live in a huge fixed array inside OrderBook, so copying or
    // moving them would corrupt the intrusive pointers
    PriceLevel(const PriceLevel&)            = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;
    PriceLevel(PriceLevel&&)                 = delete;
    PriceLevel& operator=(PriceLevel&&)      = delete;

    // append to the back of the queue (newest = lowest time priority)
    // expects order->prev == order->next == nullptr
    void push_back(Order* order) noexcept {
        order->prev = tail_;
        order->next = nullptr;

        if (tail_) {
            tail_->next = order;
        } else {
            head_ = order;  // list was empty
        }
        tail_ = order;

        total_quantity_ += order->remaining_quantity();
        ++order_count_;
    }

    // unlink an order anywhere in the queue in O(1), given its pointer
    // this is what makes cancels fast: hash map lookup, then direct unlink
    void remove(Order* order) noexcept {
        if (order->prev) {
            order->prev->next = order->next;
        } else {
            head_ = order->next;  // order was the head
        }

        if (order->next) {
            order->next->prev = order->prev;
        } else {
            tail_ = order->prev;  // order was the tail
        }

        order->prev = nullptr;
        order->next = nullptr;

        total_quantity_ -= order->remaining_quantity();
        --order_count_;
    }

    // remove and return the oldest order, or nullptr if empty
    Order* pop_front() noexcept {
        if (!head_) return nullptr;

        Order* order = head_;
        head_ = order->next;

        if (head_) {
            head_->prev = nullptr;
        } else {
            tail_ = nullptr;
        }

        total_quantity_ -= order->remaining_quantity();
        --order_count_;

        order->prev = nullptr;
        order->next = nullptr;

        return order;
    }

    // keep the aggregate in sync after a partial fill - the caller updates
    // the order's filled_quantity, then subtracts the filled amount here
    void reduce_quantity(Quantity qty) noexcept {
        total_quantity_ -= qty;
    }

    // oldest order (highest time priority), or nullptr if empty
    [[nodiscard]] Order* front() const noexcept { return head_; }

    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

    // aggregate remaining quantity, maintained incrementally so reading it
    // is O(1) instead of a list walk
    [[nodiscard]] Quantity total_quantity() const noexcept { return total_quantity_; }

    [[nodiscard]] uint32_t order_count() const noexcept { return order_count_; }

private:
    Order*   head_           = nullptr;  // oldest order
    Order*   tail_           = nullptr;  // newest order
    Quantity total_quantity_ = 0;
    uint32_t order_count_    = 0;
};

} // namespace engine
