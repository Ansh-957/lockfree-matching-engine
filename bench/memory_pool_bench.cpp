// memory_pool_bench.cpp - MemoryPool vs new/delete, measured honestly.
//
// The previous version of this file reported 0.22ns "allocations" - a red
// flag, since that is about one CPU cycle. The cause: allocate() followed
// immediately by deallocate() of the same pointer restores the pool to its
// exact prior state, and both functions inline, so the optimizer folded the
// whole pair to nearly nothing. A benchmark that measures deleted code.
//
// The fix is a rolling window of live allocations: each iteration frees the
// OLDEST pointer and allocates a new one. The pool's free-list head really
// changes every iteration, the window array is written and reread, and with
// 256 in-flight Orders the working set also looks like the real engine
// (many live orders, LIFO-ish reuse) instead of hammering one hot slot.
// The same shape is used for new/delete so the comparison stays fair.

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>

#include "core/memory_pool.h"
#include "core/order.h"
#include "core/types.h"

namespace {

constexpr std::size_t kPoolCapacity = 1 << 16;
constexpr std::size_t kWindow       = 256;  // live allocations at all times

void BM_PoolCycle_Windowed(benchmark::State& state) {
    engine::MemoryPool<engine::Order, kPoolCapacity> pool;

    std::array<engine::Order*, kWindow> window{};
    for (auto& slot : window) slot = pool.allocate();

    std::size_t i = 0;
    for (auto _ : state) {
        pool.deallocate(window[i]);
        window[i] = pool.allocate();
        benchmark::DoNotOptimize(window[i]);
        i = (i + 1) & (kWindow - 1);
        benchmark::ClobberMemory();
    }

    for (auto* slot : window) pool.deallocate(slot);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
    state.SetLabel("pool");
}
BENCHMARK(BM_PoolCycle_Windowed);

void BM_NewDeleteCycle_Windowed(benchmark::State& state) {
    std::array<engine::Order*, kWindow> window{};
    for (auto& slot : window) slot = new engine::Order;

    std::size_t i = 0;
    for (auto _ : state) {
        delete window[i];
        window[i] = new engine::Order;
        benchmark::DoNotOptimize(window[i]);
        i = (i + 1) & (kWindow - 1);
        benchmark::ClobberMemory();
    }

    for (auto* slot : window) delete slot;
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
    state.SetLabel("new/delete");
}
BENCHMARK(BM_NewDeleteCycle_Windowed);

} // namespace
