// Unit tests for engine::SPSCQueue
//
// Single-threaded correctness (FIFO, full/empty edges, wrap-around) plus a
// two-thread stress test that pushes 1M items through the queue and checks
// every one arrives exactly once, in order

#include <gtest/gtest.h>
#include "transport/spsc_queue.h"

#include <cstdint>
#include <thread>
#include <vector>

namespace {
// usable slots = Capacity - 1 (one reserved to distinguish full from empty)
constexpr std::size_t kSmallCapacity = 16;
constexpr std::size_t kStressCapacity = 1024;  // small on purpose: forces wrap-arounds and full/empty collisions under contention
} // namespace

TEST(SPSCQueueTest, PushAndPop) {
    engine::SPSCQueue<int, kSmallCapacity> q;

    EXPECT_TRUE(q.empty());
    ASSERT_TRUE(q.try_push(42));
    EXPECT_EQ(q.size(), 1u);

    int val = 0;
    ASSERT_TRUE(q.try_pop(val));
    EXPECT_EQ(val, 42);
    EXPECT_TRUE(q.empty());
}

TEST(SPSCQueueTest, EmptyPopReturnsFalse) {
    engine::SPSCQueue<int, kSmallCapacity> q;

    int val = -1;
    EXPECT_FALSE(q.try_pop(val));
    EXPECT_EQ(val, -1);  // untouched on failure
}

TEST(SPSCQueueTest, FullPushReturnsFalse) {
    engine::SPSCQueue<int, kSmallCapacity> q;

    const std::size_t usable = kSmallCapacity - 1;
    for (std::size_t i = 0; i < usable; ++i) {
        ASSERT_TRUE(q.try_push(static_cast<int>(i))) << "push failed at " << i;
    }

    EXPECT_FALSE(q.try_push(999));
    EXPECT_EQ(q.size(), usable);

    // popping one slot makes room for exactly one push
    int val = 0;
    ASSERT_TRUE(q.try_pop(val));
    EXPECT_TRUE(q.try_push(999));
    EXPECT_FALSE(q.try_push(1000));
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

TEST(SPSCQueueTest, WrapAround) {
    // cycle far more items than the capacity through a small queue so the
    // indices wrap the ring many times
    engine::SPSCQueue<int, kSmallCapacity> q;

    int next_push = 0;
    int next_pop  = 0;
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (int i = 0; i < 10; ++i) {
            ASSERT_TRUE(q.try_push(next_push++));
        }
        for (int i = 0; i < 10; ++i) {
            int val = -1;
            ASSERT_TRUE(q.try_pop(val));
            ASSERT_EQ(val, next_pop++);
        }
    }
    EXPECT_TRUE(q.empty());
}

TEST(SPSCQueueTest, ConcurrentProducerConsumer) {
    // one producer, one consumer, 1M items through a deliberately small
    // ring - constant wrap-around plus full/empty boundary hits is the
    // hostile case for the memory-ordering logic
    constexpr std::uint64_t kItems = 1'000'000;
    engine::SPSCQueue<std::uint64_t, kStressCapacity> q;

    std::vector<std::uint64_t> received;
    received.reserve(kItems);

    std::thread consumer([&] {
        std::uint64_t val = 0;
        for (std::uint64_t i = 0; i < kItems;) {
            if (q.try_pop(val)) {
                received.push_back(val);
                ++i;
            }
            // busy-spin when empty - fine for a test
        }
    });

    for (std::uint64_t i = 0; i < kItems;) {
        if (q.try_push(i)) {
            ++i;
        }
        // busy-retry when full
    }

    consumer.join();

    ASSERT_EQ(received.size(), kItems);
    for (std::uint64_t i = 0; i < kItems; ++i) {
        if (received[i] != i) {
            FAIL() << "order mismatch at index " << i << ": got " << received[i];
        }
    }
    EXPECT_TRUE(q.empty());
}
