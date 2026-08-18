#pragma once

// Lock-free single-producer single-consumer bounded ring buffer - the
// communication channel between the engine's threads
//
// Lamport queue with two refinements:
//   - cache-line padding so the producer's head_ and consumer's tail_
//     never share a line (no false sharing ping-pong)
//   - each side keeps a non-atomic cache of the other side's index and
//     only reloads it when the queue looks full/empty, so most pushes and
//     pops touch no shared cache line at all
//
// Memory ordering contract:
//   - producer: store head_ with release AFTER writing the slot, so the
//     consumer's acquire load of head_ guarantees it sees the item
//   - consumer: store tail_ with release AFTER reading the slot, so the
//     producer's acquire load of tail_ knows the slot is safe to overwrite
//
// Capacity must be a power of 2: index wrapping is a bitwise AND instead
// of a modulo. One slot is always left unused to tell full from empty,
// so usable capacity is Capacity - 1

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace engine {

template <typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of 2 for efficient index wrapping");
    static_assert(Capacity > 1, "Capacity must be at least 2 (one slot is reserved)");

    // slots are copied in and out concurrently with no synchronization on
    // the element itself - only safe for trivially copyable types
    static_assert(std::is_trivially_copyable_v<T>,
        "T must be trivially copyable (messages must be fixed-size POD)");

public:
    // heap-backed: a queue of 64K 48-byte messages is ~3MB, far too big to
    // live inside the object if anyone puts one on the stack
    SPSCQueue() : buffer_(std::make_unique<T[]>(Capacity)) {}

    // shared between threads by reference; moving would tear the indices
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&)                 = delete;
    SPSCQueue& operator=(SPSCQueue&&)      = delete;

    // producer thread only
    bool try_push(const T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & kIndexMask;

        // queue looks full against our cached view of the consumer - reload
        // the real tail once before giving up
        if (next == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (next == tail_cache_) {
                return false;  // genuinely full
            }
        }

        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // consumer thread only
    bool try_pop(T& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail == head_cache_) {
                return false;  // genuinely empty
            }
        }

        item = buffer_[tail];
        tail_.store((tail + 1) & kIndexMask, std::memory_order_release);
        return true;
    }

    // racy snapshot - monitoring only, never control flow
    [[nodiscard]] size_t size() const noexcept {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & kIndexMask;
    }

    // racy snapshot - monitoring only
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

private:
    static constexpr size_t kIndexMask = Capacity - 1;

    // producer's cache line: the write index it owns, plus its private
    // cache of the consumer's index
    alignas(64) std::atomic<size_t> head_{0};
    size_t tail_cache_{0};

    // consumer's cache line, symmetric
    alignas(64) std::atomic<size_t> tail_{0};
    size_t head_cache_{0};

    // own line: read by both sides on every operation, so it must not share
    // a line with tail_ - otherwise every consumer index bump would evict
    // the producer's copy of this pointer (and vice versa)
    alignas(64) const std::unique_ptr<T[]> buffer_;
};

} // namespace engine
