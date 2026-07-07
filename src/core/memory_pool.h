#pragma once

/// @file memory_pool.h
/// @brief Fixed-size, pre-allocated memory pool with O(1) alloc/dealloc.
///
/// The pool pre-allocates a single contiguous block of aligned memory and
/// manages it via a singly-linked free list threaded through the unused slots
/// themselves. This means:
///   - Zero heap allocations after construction.
///   - O(1) allocate and deallocate (just pop/push on the free list).
///   - No fragmentation — all slots are the same size.
///   - Cache-friendly — objects are densely packed.
///
/// The caller is responsible for placement-new after allocate() and explicit
/// destructor call before deallocate(). This is intentional: it gives the
/// matching engine full control over object lifetime.
///
/// The free list node is stored directly in the unused slot's memory using
/// a reinterpret_cast. This requires sizeof(T) >= sizeof(void*), which is
/// enforced by a static_assert.

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>

namespace engine {

/// @brief A fixed-size pool that pre-allocates PoolSize slots of sizeof(T) bytes.
///
/// @tparam T         The type to be stored. sizeof(T) must be >= sizeof(void*).
/// @tparam PoolSize  Number of slots to pre-allocate.
///
/// Usage:
/// @code
///   MemoryPool<Order, 1'000'000> pool;
///   Order* o = pool.allocate();         // Raw memory, NOT constructed
///   new (o) Order{.id = 1, ...};        // Placement new to construct
///   o->~Order();                        // Explicit destructor call
///   pool.deallocate(o);                 // Return to pool
/// @endcode
template<typename T, size_t PoolSize>
class MemoryPool {
    static_assert(sizeof(T) >= sizeof(void*),
        "sizeof(T) must be >= sizeof(void*) so the free-list pointer fits in each slot");
    static_assert(PoolSize > 0, "PoolSize must be greater than zero");

public:
    MemoryPool() {
        build_free_list();
    }

    // Non-copyable, non-movable — owns a large fixed buffer.
    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&)                 = delete;
    MemoryPool& operator=(MemoryPool&&)      = delete;

    ~MemoryPool() = default;

    /// @brief Allocate one slot from the pool.
    /// @return Pointer to raw, uninitialized memory of sizeof(T) bytes.
    ///         Caller must use placement new to construct an object.
    /// @throws std::bad_alloc if the pool is exhausted.
    [[nodiscard]] T* allocate() {
        if (!free_head_) {
            throw std::bad_alloc();
        }

        // Pop the head of the free list.
        FreeNode* node = free_head_;
        free_head_ = node->next;
        --available_;

        return reinterpret_cast<T*>(node);
    }

    /// @brief Return a slot to the pool.
    /// @param ptr  Pointer previously returned by allocate(). The caller must
    ///             have already called the destructor on the object.
    /// @pre ptr was obtained from this pool's allocate() and is not currently free.
    void deallocate(T* ptr) noexcept {
        // Push onto the head of the free list.
        auto* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_head_;
        free_head_ = node;
        ++available_;
    }

    /// @brief Number of slots currently available for allocation.
    [[nodiscard]] size_t available() const noexcept { return available_; }

    /// @brief Total capacity of the pool (compile-time constant).
    [[nodiscard]] static constexpr size_t capacity() noexcept { return PoolSize; }

private:
    /// @brief Free-list node. Stored directly inside each unused slot.
    ///        Since sizeof(T) >= sizeof(void*), this always fits.
    struct FreeNode {
        FreeNode* next;
    };

    /// @brief Build the initial free list linking all slots together.
    void build_free_list() noexcept {
        free_head_ = nullptr;

        // Link slots in reverse order so that the first allocation returns
        // the first slot (lowest address). This improves cache locality for
        // the initial burst of allocations.
        for (size_t i = PoolSize; i > 0; --i) {
            auto* node = reinterpret_cast<FreeNode*>(slot_ptr(i - 1));
            node->next = free_head_;
            free_head_ = node;
        }

        available_ = PoolSize;
    }

    /// @brief Get a pointer to the i-th slot in the storage array.
    [[nodiscard]] std::byte* slot_ptr(size_t index) noexcept {
        return &storage_[index * sizeof(T)];
    }

    // -------------------------------------------------------------------
    // Storage
    // -------------------------------------------------------------------

    /// The backing storage. Aligned to alignof(T) so placement new is safe.
    /// We use a raw byte array rather than T[] to avoid default-constructing
    /// PoolSize objects (which may be expensive or impossible for non-default-
    /// constructible types).
    alignas(alignof(T)) std::byte storage_[sizeof(T) * PoolSize];

    /// Head of the singly-linked free list.
    FreeNode* free_head_ = nullptr;

    /// Number of slots currently available.
    size_t available_ = 0;
};

} // namespace engine
