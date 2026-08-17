#pragma once

/// @file memory_pool.h
/// @brief Fixed-size, pre-allocated memory pool with O(1) alloc/dealloc.
///
/// The pool makes exactly ONE heap allocation (in the constructor) and then
/// hands out fixed-size slots from a free list threaded through the unused
/// slots themselves. After construction there are zero calls to malloc/new
/// on the allocation path — this is the "zero-malloc hot path" guarantee.
///
/// Semantics:
///   - allocate() returns RAW, uninitialized memory. The caller constructs
///     the object with placement new:   new (ptr) Order{...};
///   - Before deallocate(), the caller runs the destructor:  ptr->~Order();
///   - allocate() returns nullptr when the pool is exhausted. No exceptions
///     are thrown on the hot path.

#include <cstddef>
#include <memory>
#include <new>

namespace engine {

template <typename T, size_t PoolSize>
class MemoryPool {
    // The free list stores a pointer inside each unused slot, so every slot
    // must be big enough to hold a pointer.
    static_assert(sizeof(T) >= sizeof(void*),
        "sizeof(T) must be >= sizeof(void*) so the free-list pointer fits in each slot");

    // storage_ comes from plain operator new[], which only guarantees
    // alignment up to __STDCPP_DEFAULT_NEW_ALIGNMENT__ (usually 16 bytes).
    static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
        "T requires stricter alignment than operator new provides");

    static_assert(PoolSize > 0, "PoolSize must be greater than zero");

public:
    // One heap allocation for the whole lifetime of the pool, then thread
    // the free list through the slots (in reverse so the first allocate()
    // returns the lowest address — friendlier for the cache on warm-up).
    MemoryPool()
        : storage_(std::make_unique<std::byte[]>(sizeof(T) * PoolSize)) {
        for (size_t i = PoolSize; i > 0; --i) {
            auto* node = reinterpret_cast<FreeNode*>(slot_ptr(i - 1));
            node->next = free_head_;
            free_head_ = node;
        }
    }

    // Non-copyable, non-movable: outstanding pointers into storage_ must
    // stay valid for the pool's whole lifetime.
    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&)                 = delete;
    MemoryPool& operator=(MemoryPool&&)      = delete;

    /// Pop a slot off the free list.
    /// @return Raw uninitialized memory for one T, or nullptr if exhausted.
    [[nodiscard]] T* allocate() noexcept {
        if (free_head_ == nullptr) {
            return nullptr;
        }
        FreeNode* node = free_head_;
        free_head_ = node->next;
        --available_;
        return reinterpret_cast<T*>(node);
    }

    /// Push a slot back onto the free list.
    /// @pre ptr came from this pool's allocate() and the object's destructor
    ///      has already been run.
    void deallocate(T* ptr) noexcept {
        auto* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_head_;
        free_head_ = node;
        ++available_;
    }

    /// Number of slots currently free.
    [[nodiscard]] size_t available() const noexcept { return available_; }

    /// Total number of slots (compile-time constant).
    [[nodiscard]] static constexpr size_t capacity() noexcept { return PoolSize; }

private:
    struct FreeNode {
        FreeNode* next;
    };

    // Convert a slot index into an address inside the raw byte buffer.
    // We deal in raw bytes (not T[]) so that no T objects are constructed
    // until the caller placement-news into a slot.
    [[nodiscard]] std::byte* slot_ptr(size_t index) noexcept {
        return &storage_[index * sizeof(T)];
    }

    // Heap-backed, NOT an inline member array: a pool of 1M Orders is ~64MB,
    // which would instantly overflow the stack if the pool were a local
    // variable. One new[] at construction is acceptable — the zero-malloc
    // guarantee applies after initialization.
    std::unique_ptr<std::byte[]> storage_;

    FreeNode* free_head_ = nullptr;
    size_t    available_ = PoolSize;
};

} // namespace engine
