// matching_bench.cpp — End-to-end matching engine benchmarks
// Linked against benchmark::benchmark_main via top-level CMakeLists.txt.

#include <benchmark/benchmark.h>
#include "core/matching_engine.h"
#include "core/order_book.h"
#include "core/types.h"

// ---------------------------------------------------------------------------
// Benchmarks — TODO stubs
// ---------------------------------------------------------------------------

static void BM_E2E_Latency(benchmark::State& state) {
    // TODO: Setup
    //   - Construct a MatchingEngine.
    //   - Pre-build ask side with multiple levels.
    //   - Prepare a vector of crossing bid orders.
    //
    // Hot loop:
    //   - Submit one crossing bid, measure time from submission to fill
    //     callback / trade event.
    //   - Replenish the ask side between iterations (pause timing).
    //
    // Report:
    //   - state.SetLabel("p50 / p99") if collecting distribution manually.
    for (auto _ : state) {
        // benchmark::DoNotOptimize(...);
    }
}
BENCHMARK(BM_E2E_Latency);

static void BM_Throughput_Sustained(benchmark::State& state) {
    // TODO: Setup
    //   - Construct a MatchingEngine with a pool allocator.
    //   - Generate a large order stream (mixed add/cancel/cross).
    //
    // Hot loop:
    //   - Feed the entire stream through the engine.
    //   - Measure total wall-clock time.
    //
    // Report:
    //   - state.SetItemsProcessed(total_orders);
    //   - Derive orders/second from benchmark output.
    for (auto _ : state) {
        // benchmark::DoNotOptimize(...);
    }
}
BENCHMARK(BM_Throughput_Sustained);

static void BM_StdMap_Baseline(benchmark::State& state) {
    // TODO: Setup
    //   - Implement a trivial matching engine that uses std::map<Price, Level>
    //     for comparison against the production intrusive-list / flat-map
    //     implementation.
    //   - Same order stream as BM_Throughput_Sustained.
    //
    // Hot loop:
    //   - Feed orders through the std::map engine.
    //
    // Report:
    //   - Compare ns/op with the production engine to quantify improvement.
    for (auto _ : state) {
        // benchmark::DoNotOptimize(...);
    }
}
BENCHMARK(BM_StdMap_Baseline);
