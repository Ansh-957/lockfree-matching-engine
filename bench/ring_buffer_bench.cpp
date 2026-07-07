// ring_buffer_bench.cpp — Benchmarks for engine::SPSCQueue
// Linked against benchmark::benchmark_main via top-level CMakeLists.txt.
// Fully implemented — SPSCQueue is complete.

#include <benchmark/benchmark.h>
#include "transport/spsc_queue.h"

#include <cstdint>
#include <thread>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {
constexpr std::size_t kCapacity = 1 << 16;  // 65 536 slots
} // namespace

// ---------------------------------------------------------------------------
// BM_SPSC_PushPop_SingleThread
// Baseline: push one item then pop it, single-threaded.
// Measures raw ring-buffer overhead without any contention.
// ---------------------------------------------------------------------------
static void BM_SPSC_PushPop_SingleThread(benchmark::State& state) {
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

// ---------------------------------------------------------------------------
// BM_SPSC_Throughput_TwoThreads
// Producer/consumer on separate threads, measuring throughput.
// The benchmark loop runs on the producer thread; the consumer spins
// independently and we measure wall-clock time across both.
// ---------------------------------------------------------------------------
static void BM_SPSC_Throughput_TwoThreads(benchmark::State& state) {
    const auto items_per_iter = static_cast<std::size_t>(state.range(0));
    engine::SPSCQueue<std::uint64_t, kCapacity> q;

    for (auto _ : state) {
        // Consumer thread
        std::thread consumer([&] {
            std::uint64_t tmp = 0;
            for (std::size_t i = 0; i < items_per_iter; /* no incr */) {
                if (q.try_pop(tmp)) {
                    benchmark::DoNotOptimize(tmp);
                    ++i;
                }
            }
        });

        // Producer (benchmark thread)
        for (std::uint64_t i = 0; i < items_per_iter; /* no incr */) {
            if (q.try_push(i)) {
                ++i;
            }
        }

        consumer.join();
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(items_per_iter));
}
BENCHMARK(BM_SPSC_Throughput_TwoThreads)
    ->Arg(1 << 14)   //  16 384 items
    ->Arg(1 << 16)   //  65 536 items
    ->Arg(1 << 18)   // 262 144 items
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();
