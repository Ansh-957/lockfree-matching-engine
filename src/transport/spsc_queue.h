#pragma once

/// @file spsc_queue.h
/// @brief Lock-free single-producer single-consumer bounded queue.
///
/// This is the primary communication channel between threads in the engine.
/// The design is based on the classic Lamport SPSC queue with cache-line
/// padding to prevent false sharing between the producer's head and the
/// consumer's tail.
///
/// Memory ordering:
///   - Producer loads its own head with relaxed, loads tail with acquire,
///     stores head with release. This ensures the consumer sees the written
///     item before the updated head.
///   - Consumer loads its own tail with relaxed, loads head with acquire,
///     stores tail with release. Symmetric reasoning.
///
/// Capacity must be a power of 2 so we can use bitwise AND for index wrapping
/// instead of the expensive modulo operator.

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace engine {

/// @brief A bounded, lock-free SPSC (single-producer, single-consumer) queue.
///
/// @tparam T         Element type. Should be trivially copyable for best perf.
/// @tparam Capacity  Maximum number of elements. Must be a power of 2.
///
/// Usage:
/// @code
///   SPSCQueue<EngineMessage, 65536> queue;
///
///   // Producer thread:
///   queue.try_push(msg);
///
///   // Consumer thread:
///   EngineMessage msg;
///   if (queue.try_pop(msg)) { /* process msg */ }
/// @endcode
template<typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of 2 for efficient index wrapping");
    static_assert(Capacity > 0, "Capacity must be greater than zero");

public:
    SPSCQueue() = default;

    // Non-copyable, non-movable — shared between threads via reference.
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&)                 = delete;
    SPSCQueue& operator=(SPSCQueue&&)      = delete;

    /// @brief Try to enqueue an item (producer side only).
    /// @param item  The item to enqueue.
    /// @return true if the item was enqueued, false if the queue is full.
    ///
    /// Called by exactly ONE producer thread. Never call from the consumer.
    bool try_push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t next_head    = (current_head + 1) & kIndexMask;

        // Check if queue is full: next write position would collide with tail.
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Queue is full
        }

        buffer_[current_head] = item;

        // Release ensures the item write above is visible before the consumer
        // sees the updated head.
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    /// @brief Try to dequeue an item (consumer side only).
    /// @param[out] item  The dequeued item is written here on success.
    /// @return true if an item was dequeued, false if the queue is empty.
    ///
    /// Called by exactly ONE consumer thread. Never call from the producer.
    bool try_pop(T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        // Check if queue is empty: tail has caught up to head.
        if (current_tail == head_.load(std::memory_order_acquire)) {
            return false;  // Queue is empty
        }

        item = buffer_[current_tail];

        const size_t next_tail = (current_tail + 1) & kIndexMask;

        // Release ensures the item read above completes before the producer
        // sees the updated tail and potentially overwrites the slot.
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    /// @brief Approximate size of the queue.
    ///
    /// This is inherently racy in a multi-threaded context — the returned
    /// value may be stale by the time the caller uses it. Useful only for
    /// monitoring and diagnostics, NOT for control flow decisions.
    [[nodiscard]] size_t size() const noexcept {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & kIndexMask;
    }

    /// @brief Check if the queue appears empty (racy, for diagnostics only).
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_acquire);
    }

    /// @brief Maximum number of elements the queue can hold.
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        // Usable capacity is Capacity - 1 because one slot is always unused
        // (to distinguish full from empty), but we report the template arg.
        return Capacity;
    }

private:
    /// Bitmask for fast modulo: index & kIndexMask == index % Capacity.
    static constexpr size_t kIndexMask = Capacity - 1;

    // -------------------------------------------------------------------
    // Cache-line-padded indices to prevent false sharing.
    //
    // The producer owns head_ and the consumer owns tail_. By placing them
    // on separate cache lines (64 bytes on x86), writes to one do not
    // invalidate the cache line holding the other.
    // -------------------------------------------------------------------

    /// Producer write index. Only the producer thread writes to this.
    alignas(64) std::atomic<size_t> head_{0};

    /// Consumer read index. Only the consumer thread writes to this.
    alignas(64) std::atomic<size_t> tail_{0};

    /// Ring buffer storage. Sits after the padded indices.
    std::array<T, Capacity> buffer_{};
};

} // namespace engine
