// order_book_bench.cpp — Benchmarks for engine::OrderBook
// Linked against benchmark::benchmark_main via top-level CMakeLists.txt.

#include <benchmark/benchmark.h>
#include "core/order_book.h"
#include "core/types.h"

// ---------------------------------------------------------------------------
// Benchmarks — TODO stubs
// ---------------------------------------------------------------------------

static void BM_AddOrder(benchmark::State& state) {
    // TODO: Setup
    //   - Construct an OrderBook.
    //   - Pre-generate a vector of Order structs with ascending order IDs and
    //     random prices drawn from a narrow band (e.g., 99.00–101.00).
    //
    // Hot loop:
    //   - For each iteration, add one order to the book.
    //   - Use state.PauseTiming() / ResumeTiming() around any per-iteration
    //     setup if needed (e.g., resetting the book after it fills up).
    //
    // Report: orders/second via state.SetItemsProcessed().
    for (auto _ : state) {
        // benchmark::DoNotOptimize(...);
    }
}
BENCHMARK(BM_AddOrder);

static void BM_CancelOrder(benchmark::State& state) {
    // TODO: Setup
    //   - Construct an OrderBook pre-populated with N orders.
    //   - Store their order IDs in a vector.
    //
    // Hot loop:
    //   - Cancel orders one by one.
    //   - Re-add if the vector is exhausted (pause timing for re-add).
    for (auto _ : state) {
        // benchmark::DoNotOptimize(...);
    }
}
BENCHMARK(BM_CancelOrder);

static void BM_MatchOrder(benchmark::State& state) {
    // TODO: Setup
    //   - Build a book with multiple ask levels.
    //   - Prepare crossing bid orders.
    //
    // Hot loop:
    //   - Submit a crossing order, measure time to match.
    //   - Replenish the ask side after each iteration.
    for (auto _ : state) {
        // benchmark::DoNotOptimize(...);
    }
}
BENCHMARK(BM_MatchOrder);

static void BM_AddCancel_PoolVsMalloc(benchmark::State& state) {
    // TODO: Setup
    //   - Parameterize with state.range(0): 0 = pool alloc, 1 = new/delete.
    //   - Create OrderBook (pool variant) or use raw new Order / delete.
    //
    // Hot loop:
    //   - Allocate an order, add to book, cancel it, deallocate.
    //   - Compare pool allocator vs. global new/delete overhead.
    for (auto _ : state) {
        // benchmark::DoNotOptimize(...);
    }
}
BENCHMARK(BM_AddCancel_PoolVsMalloc)->Arg(0)->Arg(1);
