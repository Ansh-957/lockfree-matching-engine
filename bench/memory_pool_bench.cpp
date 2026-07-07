// memory_pool_bench.cpp — Benchmarks for engine::MemoryPool
// Linked against benchmark::benchmark_main via top-level CMakeLists.txt.
// Fully implemented — MemoryPool is complete.

#include <benchmark/benchmark.h>
#include "core/memory_pool.h"
#include "core/order.h"
#include "core/types.h"

#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {
constexpr std::size_t kPoolCapacity = 1 << 16;  // 65 536 slots
} // namespace

// ---------------------------------------------------------------------------
// BM_PoolAllocate
// Measure the cost of a single allocation from a pre-warmed pool.
// ---------------------------------------------------------------------------
static void BM_PoolAllocate(benchmark::State& state) {
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    // Pre-warm: allocate one and immediately return it so the free-list is hot.
    auto* warm = pool.allocate();
    pool.deallocate(warm);

    for (auto _ : state) {
        auto* p = pool.allocate();
        benchmark::DoNotOptimize(p);
        // Return so we can allocate again next iteration
        pool.deallocate(p);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_PoolAllocate);

// ---------------------------------------------------------------------------
// BM_PoolDeallocate
// Measure the cost of returning a slot to the free-list.
// ---------------------------------------------------------------------------
static void BM_PoolDeallocate(benchmark::State& state) {
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    for (auto _ : state) {
        state.PauseTiming();
        auto* p = pool.allocate();
        state.ResumeTiming();

        pool.deallocate(p);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_PoolDeallocate);

// ---------------------------------------------------------------------------
// BM_MallocAllocate
// Baseline: measure new / delete for comparison.
// ---------------------------------------------------------------------------
static void BM_MallocAllocate(benchmark::State& state) {
    for (auto _ : state) {
        auto* p = new engine::Order;
        benchmark::DoNotOptimize(p);
        delete p;
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_MallocAllocate);

// ---------------------------------------------------------------------------
// BM_PoolVsMalloc_Cycle
// Allocate + deallocate cycle: pool vs malloc.
// Parameterized: Arg(0) = pool, Arg(1) = new/delete.
// ---------------------------------------------------------------------------
static void BM_PoolVsMalloc_Cycle(benchmark::State& state) {
    const bool use_pool = (state.range(0) == 0);

    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    for (auto _ : state) {
        if (use_pool) {
            auto* p = pool.allocate();
            benchmark::DoNotOptimize(p);
            pool.deallocate(p);
        } else {
            auto* p = new engine::Order;
            benchmark::DoNotOptimize(p);
            delete p;
        }
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
    state.SetLabel(use_pool ? "pool" : "malloc");
}
BENCHMARK(BM_PoolVsMalloc_Cycle)->Arg(0)->Arg(1);
