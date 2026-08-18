#pragma once

// Fixed-size pre-allocated memory pool with O(1) alloc/dealloc
//
// One heap allocation in the constructor, then slots are handed out from a
// free list threaded through the unused slots themselves - zero malloc on
// the allocation path after startup
//
// allocate() returns RAW uninitialized memory; the caller placement-news
// the object and runs the destructor before deallocate(). Returns nullptr
// when exhausted - no exceptions on the hot path

#include <cstddef>
#include <memory>
#include <new>

namespace engine {

template <typename T, size_t PoolSize>
class MemoryPool {
    // the free list stores a pointer inside each unused slot
    static_assert(sizeof(T) >= sizeof(void*),
        "sizeof(T) must be >= sizeof(void*) so the free-list pointer fits in each slot");

    // plain operator new[] only guarantees alignment up to
    // __STDCPP_DEFAULT_NEW_ALIGNMENT__ (usually 16 bytes)
    static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
        "T requires stricter alignment than operator new provides");

    static_assert(PoolSize > 0, "PoolSize must be greater than zero");

public:
    // link the free list in reverse so the first allocate() returns the
    // lowest address - the initial burst of allocations then walks memory
    // front to back, which the hardware prefetcher likes
    MemoryPool()
        : storage_(std::make_unique<std::byte[]>(sizeof(T) * PoolSize)) {
        for (size_t i = PoolSize; i > 0; --i) {
            auto* node = reinterpret_cast<FreeNode*>(slot_ptr(i - 1));
            node->next = free_head_;
            free_head_ = node;
        }
    }

    // outstanding pointers into storage_ must stay valid for the pool's
    // whole lifetime, so copying/moving is forbidden
    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&)                 = delete;
    MemoryPool& operator=(MemoryPool&&)      = delete;

    // pop a slot off the free list; nullptr if exhausted
    [[nodiscard]] T* allocate() noexcept {
        if (free_head_ == nullptr) {
            return nullptr;
        }
        FreeNode* node = free_head_;
        free_head_ = node->next;
        --available_;
        return reinterpret_cast<T*>(node);
    }

    // push a slot back onto the free list
    // caller must have already run the object's destructor
    void deallocate(T* ptr) noexcept {
        auto* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_head_;
        free_head_ = node;
        ++available_;
    }

    [[nodiscard]] size_t available() const noexcept { return available_; }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return PoolSize; }

private:
    struct FreeNode {
        FreeNode* next;
    };

    // raw bytes, not T[], so no T objects are constructed until the caller
    // placement-news into a slot
    [[nodiscard]] std::byte* slot_ptr(size_t index) noexcept {
        return &storage_[index * sizeof(T)];
    }

    // heap-backed, NOT an inline member array: a pool of 1M Orders is ~64MB
    // and would overflow the stack if the pool were a local variable
    std::unique_ptr<std::byte[]> storage_;

    FreeNode* free_head_ = nullptr;
    size_t    available_ = PoolSize;
};

} // namespace engine
