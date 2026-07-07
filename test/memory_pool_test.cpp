// memory_pool_test.cpp — Unit tests for engine::MemoryPool
// Built from top-level CMakeLists.txt as part of the unit_tests target.

#include <gtest/gtest.h>
#include "core/memory_pool.h"
#include "core/order.h"
#include "core/types.h"

#include <cstring>
#include <new>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {
constexpr std::size_t kPoolCapacity = 1024;
} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(MemoryPoolTest, AllocateAndDeallocate) {
    // Allocate one item, verify non-null, deallocate.
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    auto* ptr = pool.allocate();
    ASSERT_NE(ptr, nullptr);

    pool.deallocate(ptr);
}

TEST(MemoryPoolTest, PoolExhaustion) {
    // Allocate all kPoolCapacity slots, verify every pointer is non-null.
    // Attempt one more allocation — should throw std::bad_alloc.
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    std::vector<engine::Order*> ptrs;
    ptrs.reserve(kPoolCapacity);

    for (std::size_t i = 0; i < kPoolCapacity; ++i) {
        auto* p = pool.allocate();
        ASSERT_NE(p, nullptr) << "Allocation failed at slot " << i;
        ptrs.push_back(p);
    }

    // Pool should be exhausted — allocate() throws std::bad_alloc
    EXPECT_THROW(pool.allocate(), std::bad_alloc);
}

TEST(MemoryPoolTest, ReuseAfterDeallocation) {
    // Exhaust the pool, then deallocate one item.
    // Re-allocate — should succeed and return non-null.
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    std::vector<engine::Order*> ptrs;
    ptrs.reserve(kPoolCapacity);

    for (std::size_t i = 0; i < kPoolCapacity; ++i) {
        ptrs.push_back(pool.allocate());
    }

    EXPECT_THROW(pool.allocate(), std::bad_alloc);  // exhausted

    pool.deallocate(ptrs.back());
    ptrs.pop_back();

    auto* reused = pool.allocate();
    EXPECT_NE(reused, nullptr);
}

TEST(MemoryPoolTest, AllSlotsUsable) {
    // Allocate all slots, write a unique value to each,
    // then read back and verify — no overlap or corruption.
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    std::vector<engine::Order*> ptrs;
    ptrs.reserve(kPoolCapacity);

    for (std::size_t i = 0; i < kPoolCapacity; ++i) {
        auto* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        // Use placement new to construct each order with a unique ID
        new (p) engine::Order{};
        p->id = static_cast<engine::OrderId>(i);
        ptrs.push_back(p);
    }

    // Verify no corruption — each slot should hold the ID we assigned
    for (std::size_t i = 0; i < kPoolCapacity; ++i) {
        EXPECT_EQ(ptrs[i]->id, static_cast<engine::OrderId>(i))
            << "Corruption at slot " << i;
    }

    // Clean up
    for (auto* p : ptrs) {
        p->~Order();
        pool.deallocate(p);
    }
}
