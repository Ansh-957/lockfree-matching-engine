// spsc_queue_test.cpp — Unit tests for engine::SPSCQueue
// Built from top-level CMakeLists.txt as part of the unit_tests target.
// Fully implemented — SPSCQueue is complete.

#include <gtest/gtest.h>
#include "transport/spsc_queue.h"

#include <cstdint>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {
// SPSCQueue requires power-of-2 capacity. Usable slots = Capacity - 1
// because one slot is reserved to distinguish full from empty.
constexpr std::size_t kSmallCapacity = 16;       // 15 usable slots
constexpr std::size_t kLargeCapacity = 1 << 20;  // 1 048 576 (1 048 575 usable)
} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(SPSCQueueTest, PushAndPop) {
    engine::SPSCQueue<int, kSmallCapacity> q;

    ASSERT_TRUE(q.try_push(42));

    int val = 0;
    ASSERT_TRUE(q.try_pop(val));
    EXPECT_EQ(val, 42);
}

TEST(SPSCQueueTest, EmptyPopReturnsFalse) {
    engine::SPSCQueue<int, kSmallCapacity> q;

    int val = -1;
    EXPECT_FALSE(q.try_pop(val));
    EXPECT_EQ(val, -1);  // val should be unchanged
}

TEST(SPSCQueueTest, FullPushReturnsFalse) {
    engine::SPSCQueue<int, kSmallCapacity> q;

    // Fill every usable slot (Capacity - 1 slots usable)
    const std::size_t usable = kSmallCapacity - 1;
    for (std::size_t i = 0; i < usable; ++i) {
        ASSERT_TRUE(q.try_push(static_cast<int>(i)))
            << "try_push failed at index " << i;
    }

    // Next push must fail — queue is full
    EXPECT_FALSE(q.try_push(999));
}

TEST(SPSCQueueTest, FIFO) {
    constexpr int kCount = 128;
    engine::SPSCQueue<int, 256> q;

    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(q.try_push(i));
    }

    for (int i = 0; i < kCount; ++i) {
        int val = -1;
        ASSERT_TRUE(q.try_pop(val));
        EXPECT_EQ(val, i) << "FIFO violated at position " << i;
    }
}

TEST(SPSCQueueTest, ConcurrentProducerConsumer) {
    // Stress test: one producer thread pushes 1 M items,
    // one consumer thread pops 1 M items. Verify all received in order.

    constexpr std::size_t kItems = 1'000'000;
    engine::SPSCQueue<std::uint64_t, kLargeCapacity> q;

    std::vector<std::uint64_t> received;
    received.reserve(kItems);

    // --- Consumer thread ---
    std::thread consumer([&] {
        std::uint64_t val = 0;
        for (std::size_t i = 0; i < kItems; /* no increment */) {
            if (q.try_pop(val)) {
                received.push_back(val);
                ++i;
            }
            // Busy-spin if nothing available — acceptable for testing.
        }
    });

    // --- Producer thread (runs on the test thread) ---
    for (std::uint64_t i = 0; i < kItems; /* no increment */) {
        if (q.try_push(i)) {
            ++i;
        }
        // Busy-retry if queue is full.
    }

    consumer.join();

    // Verify count and order
    ASSERT_EQ(received.size(), kItems);
    for (std::size_t i = 0; i < kItems; ++i) {
        EXPECT_EQ(received[i], static_cast<std::uint64_t>(i))
            << "Order mismatch at index " << i;
    }
}
