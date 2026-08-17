// memory_pool_test.cpp — Unit tests for engine::MemoryPool
//
// The pool's contract under test:
//   - allocate() returns raw memory; caller placement-news the object.
//   - allocate() returns nullptr (does NOT throw) when the pool is exhausted.
//   - deallocate() returns a slot to the pool for reuse.
//   - Slots never overlap; each allocation is a distinct, usable region.

#include <gtest/gtest.h>
#include "core/memory_pool.h"
#include "core/order.h"
#include "core/types.h"

#include <algorithm>
#include <new>
#include <vector>

namespace {
constexpr std::size_t kPoolCapacity = 1024;
} // namespace

TEST(MemoryPoolTest, AllocateAndDeallocate) {
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    EXPECT_EQ(pool.available(), kPoolCapacity);
    EXPECT_EQ(pool.capacity(), kPoolCapacity);

    auto* ptr = pool.allocate();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(pool.available(), kPoolCapacity - 1);

    pool.deallocate(ptr);
    EXPECT_EQ(pool.available(), kPoolCapacity);
}

TEST(MemoryPoolTest, PoolExhaustion) {
    // Allocate every slot, then verify the next allocation returns nullptr.
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    std::vector<engine::Order*> ptrs;
    ptrs.reserve(kPoolCapacity);

    for (std::size_t i = 0; i < kPoolCapacity; ++i) {
        auto* p = pool.allocate();
        ASSERT_NE(p, nullptr) << "Allocation failed at slot " << i;
        ptrs.push_back(p);
    }

    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.allocate(), nullptr);  // exhausted — no throw, just nullptr

    for (auto* p : ptrs) {
        pool.deallocate(p);
    }
}

TEST(MemoryPoolTest, AllPointersDistinct) {
    // No two allocations may return the same slot.
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    std::vector<engine::Order*> ptrs;
    ptrs.reserve(kPoolCapacity);
    for (std::size_t i = 0; i < kPoolCapacity; ++i) {
        ptrs.push_back(pool.allocate());
    }

    std::sort(ptrs.begin(), ptrs.end());
    auto dup = std::adjacent_find(ptrs.begin(), ptrs.end());
    EXPECT_EQ(dup, ptrs.end()) << "Pool returned the same slot twice";

    for (auto* p : ptrs) {
        pool.deallocate(p);
    }
}

TEST(MemoryPoolTest, ReuseAfterDeallocation) {
    // Exhaust the pool, free one slot, and verify it can be re-allocated.
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    std::vector<engine::Order*> ptrs;
    ptrs.reserve(kPoolCapacity);
    for (std::size_t i = 0; i < kPoolCapacity; ++i) {
        ptrs.push_back(pool.allocate());
    }

    EXPECT_EQ(pool.allocate(), nullptr);  // exhausted

    engine::Order* freed = ptrs.back();
    pool.deallocate(freed);
    ptrs.pop_back();

    auto* reused = pool.allocate();
    EXPECT_NE(reused, nullptr);
    // The free list is LIFO, so the just-freed slot should come back first.
    EXPECT_EQ(reused, freed);

    ptrs.push_back(reused);
    for (auto* p : ptrs) {
        pool.deallocate(p);
    }
}

TEST(MemoryPoolTest, AllSlotsUsable) {
    // Construct a real Order in every slot, write a unique ID, and read it
    // back — proves no slot overlap or corruption.
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    std::vector<engine::Order*> ptrs;
    ptrs.reserve(kPoolCapacity);

    for (std::size_t i = 0; i < kPoolCapacity; ++i) {
        auto* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        new (p) engine::Order{};  // placement new — caller constructs
        p->id = static_cast<engine::OrderId>(i);
        ptrs.push_back(p);
    }

    for (std::size_t i = 0; i < kPoolCapacity; ++i) {
        EXPECT_EQ(ptrs[i]->id, static_cast<engine::OrderId>(i))
            << "Corruption at slot " << i;
    }

    for (auto* p : ptrs) {
        p->~Order();  // caller destroys before returning the slot
        pool.deallocate(p);
    }
    EXPECT_EQ(pool.available(), kPoolCapacity);
}
