#pragma once

/// @file price_level.h
/// @brief Intrusive doubly-linked list of orders at a single price level.
///
/// PriceLevel is the core FIFO queue for price-time priority matching.
/// Orders are linked via their embedded prev/next pointers (see order.h),
/// so no heap allocation occurs when inserting or removing orders.
///
/// All methods are inline — they are simple pointer manipulations that
/// should compile down to a handful of instructions.

#include "core/order.h"

namespace engine {

/// @brief A FIFO queue of orders at a single price, implemented as an
///        intrusive doubly-linked list through Order::prev / Order::next.
///
/// Invariants:
///   - If the list is non-empty, head_->prev == nullptr and tail_->next == nullptr.
///   - total_quantity_ == sum of remaining_quantity() across all orders.
///   - order_count_ == number of orders in the list.
class PriceLevel {
public:
    PriceLevel() = default;

    // Non-copyable, non-movable — these live in a fixed array.
    PriceLevel(const PriceLevel&)            = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;
    PriceLevel(PriceLevel&&)                 = delete;
    PriceLevel& operator=(PriceLevel&&)      = delete;

    // -------------------------------------------------------------------
    // Modifiers
    // -------------------------------------------------------------------

    /// @brief Append an order to the back of the queue (FIFO insertion).
    /// @param order  Pointer to the order to insert. Must not be nullptr.
    ///               The order's prev/next pointers will be overwritten.
    /// @pre order != nullptr && order->prev == nullptr && order->next == nullptr
    void push_back(Order* order) noexcept {
        order->prev = tail_;
        order->next = nullptr;

        if (tail_) {
            tail_->next = order;
        } else {
            // List was empty — this order becomes the head too.
            head_ = order;
        }
        tail_ = order;

        total_quantity_ += order->remaining_quantity();
        ++order_count_;
    }

    /// @brief Remove an arbitrary order from the queue in O(1).
    /// @param order  Pointer to the order to remove. Must currently be in this list.
    ///
    /// This is where intrusive lists shine: given a direct pointer to the
    /// order, removal is O(1) with no search. This is critical for cancel
    /// performance — we look up the order by ID in a hash map and remove
    /// it from its price level in constant time.
    void remove(Order* order) noexcept {
        if (order->prev) {
            order->prev->next = order->next;
        } else {
            // order was the head
            head_ = order->next;
        }

        if (order->next) {
            order->next->prev = order->prev;
        } else {
            // order was the tail
            tail_ = order->prev;
        }

        // Clear the order's list pointers so it's no longer linked.
        order->prev = nullptr;
        order->next = nullptr;

        total_quantity_ -= order->remaining_quantity();
        --order_count_;
    }

    /// @brief Remove and return the front (oldest) order.
    /// @return Pointer to the former head, or nullptr if the list was empty.
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

    /// @brief Reduce the tracked total quantity (e.g., after a partial fill).
    /// @param qty  The quantity to subtract from total_quantity_.
    ///
    /// Call this when an order in this level is partially filled. The order's
    /// own filled_quantity is updated by the caller; this method keeps the
    /// level's aggregate in sync.
    void reduce_quantity(Quantity qty) noexcept {
        total_quantity_ -= qty;
    }

    // -------------------------------------------------------------------
    // Observers
    // -------------------------------------------------------------------

    /// @brief Return the front (oldest / highest priority) order, or nullptr.
    [[nodiscard]] Order* front() const noexcept { return head_; }

    /// @brief Is this price level empty (no resting orders)?
    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

    /// @brief Total remaining quantity across all orders at this price.
    [[nodiscard]] Quantity total_quantity() const noexcept { return total_quantity_; }

    /// @brief Number of orders resting at this price.
    [[nodiscard]] uint32_t order_count() const noexcept { return order_count_; }

private:
    Order*   head_           = nullptr;  ///< Front of FIFO queue (oldest order)
    Order*   tail_           = nullptr;  ///< Back of FIFO queue (newest order)
    Quantity total_quantity_  = 0;       ///< Aggregate remaining quantity
    uint32_t order_count_    = 0;       ///< Number of orders in this level
};

} // namespace engine
