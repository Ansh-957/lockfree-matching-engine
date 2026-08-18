// Benchmarks for the SPSC ring buffer
//
//   BM_SPSC_PushPop_SingleThread - raw instruction cost of push+pop with no
//     second thread: no cross-core traffic, upper bound on queue speed
//   BM_SPSC_Throughput - sustained producer->consumer transfer rate with a
//     persistent consumer thread. Templated on the payload type to show the
//     cost of moving 8-byte vs full-message slots across cores
//   BM_SPSC_RoundTrip - ping-pong latency through a pair of queues: one
//     round trip = two pushes, two pops, two cross-core cache-line handoffs
//
// The consumer publishes progress through a separate atomic counter, batched
// (every 1024 items or when the queue drains) so the measurement adds no
// per-item shared write to the consumer's fast path

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include "transport/message.h"
#include "transport/spsc_queue.h"

namespace {

constexpr std::size_t kCapacity = 1 << 16;  // 65,536 slots

void BM_SPSC_PushPop_SingleThread(benchmark::State& state) {
    engine::SPSCQueue<std::uint64_t, kCapacity> q;
    std::uint64_t val = 0;

    for (auto _ : state) {
        q.try_push(val);
        q.try_pop(val);
        benchmark::DoNotOptimize(val);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_SPSC_PushPop_SingleThread);

// one consumer thread lives for the whole benchmark (spawning a thread per
// iteration would distort small batches by tens of microseconds); each
// iteration transfers state.range(0) items and waits until they all arrive
template <typename T>
void BM_SPSC_Throughput(benchmark::State& state) {
    const auto items = static_cast<std::uint64_t>(state.range(0));

    engine::SPSCQueue<T, kCapacity> q;
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool> stop{false};

    std::thread consumer([&] {
        std::uint64_t local = 0;
        T tmp{};
        while (true) {
            if (q.try_pop(tmp)) {
                benchmark::DoNotOptimize(tmp);
                ++local;
                if ((local & 1023) == 0) {
                    consumed.store(local, std::memory_order_release);
                }
            } else {
                // drained: publish exact progress, then check for shutdown
                consumed.store(local, std::memory_order_release);
                if (stop.load(std::memory_order_acquire)) {
                    return;
                }
            }
        }
    });

    const T value{};
    std::uint64_t produced = 0;
    for (auto _ : state) {
        const std::uint64_t target = produced + items;
        while (produced < target) {
            if (q.try_push(value)) {
                ++produced;
            }
        }
        // iteration ends only when the consumer caught up, so timing always
        // covers the full transfer, not just filling the ring
        while (consumed.load(std::memory_order_acquire) < target) {
        }
    }

    stop.store(true, std::memory_order_release);
    consumer.join();

    state.SetItemsProcessed(static_cast<int64_t>(produced));
    state.SetBytesProcessed(static_cast<int64_t>(produced * sizeof(T)));
}
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, std::uint64_t)
    ->Arg(1 << 16)
    ->Arg(1 << 20)
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_SPSC_Throughput, engine::EngineMessage)
    ->Arg(1 << 16)
    ->Arg(1 << 20)
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

// small rings on purpose: latency, not capacity, is under test
void BM_SPSC_RoundTrip(benchmark::State& state) {
    engine::SPSCQueue<std::uint64_t, 1024> ping;
    engine::SPSCQueue<std::uint64_t, 1024> pong;
    std::atomic<bool> stop{false};

    std::thread echo([&] {
        std::uint64_t v = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            if (ping.try_pop(v)) {
                while (!pong.try_push(v)) {
                }
            }
        }
    });

    std::uint64_t v = 0;
    std::uint64_t r = 0;
    for (auto _ : state) {
        while (!ping.try_push(v)) {
        }
        while (!pong.try_pop(r)) {
        }
        benchmark::DoNotOptimize(r);
        ++v;
    }

    stop.store(true, std::memory_order_relaxed);
    echo.join();

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_SPSC_RoundTrip)->UseRealTime();

} // namespace
