// Benchmarks for the OrderBook hot paths, the pool-vs-malloc comparison,
// and the synthetic end-to-end stream through the MatchingEngine
//
// Methodology:
//   - one book/engine is constructed per benchmark and reused across
//     iterations: a fresh book is a ~240MB allocation, and constructing
//     one per iteration would swamp the numbers being measured
//   - bids and asks are generated in disjoint price bands so the
//     standalone book keeps the uncrossed invariant without a matcher
//   - periodic refill/drain work is bracketed in PauseTiming/ResumeTiming
//     so only the operation under test is on the clock

#include <benchmark/benchmark.h>

#include <algorithm>
#include <memory>
#include <new>
#include <random>
#include <vector>

#include "core/matching_engine.h"
#include "core/memory_pool.h"
#include "core/order.h"
#include "core/order_book.h"
#include "core/types.h"
#include "tools/synthetic_workload.h"

namespace {

using namespace engine;

constexpr size_t kBatch = 1 << 16;  // orders per refill batch

// disjoint bands: bids below kMid, asks above - the book stays uncrossed
// without a matching engine enforcing it
constexpr Price kMid     = 10'000;  // $100.00
constexpr Price kBidMean =  9'950;
constexpr Price kAskMean = 10'050;

// overwrite the arena with a fresh batch of resting-ready limit orders,
// alternating sides, prices normally distributed inside each side's band
void regenerate(std::vector<Order>& arena, std::mt19937_64& rng, OrderId& next_id) {
    std::normal_distribution<double> offset(0.0, 20.0);
    for (auto& o : arena) {
        const bool is_bid = (next_id & 1) == 0;
        o = Order{};
        o.id       = next_id++;
        o.side     = is_bid ? Side::Bid : Side::Ask;
        o.type     = OrderType::Limit;
        o.quantity = 100;

        const Price mean = is_bid ? kBidMean : kAskMean;
        const Price p    = mean + static_cast<Price>(offset(rng));
        o.price = is_bid ? std::clamp<Price>(p, 1, kMid - 1)
                         : std::clamp<Price>(p, kMid + 1, 2 * kMid);
    }
}

// BM_AddOrder: cost of resting one order (level append + id-map insert +
// best-price check). The book is drained and the arena regenerated every
// kBatch iterations, off the clock
void BM_AddOrder(benchmark::State& state) {
    OrderBook book;
    std::vector<Order> arena(kBatch);
    std::mt19937_64 rng(42);
    OrderId next_id = 1;
    size_t i = kBatch;

    for (auto _ : state) {
        if (i == kBatch) {
            state.PauseTiming();
            for (const auto& o : arena) {
                book.cancel_order(o.id);  // no-op on the very first batch
            }
            regenerate(arena, rng, next_id);
            i = 0;
            state.ResumeTiming();
        }
        benchmark::DoNotOptimize(book.add_order(&arena[i++]));
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_AddOrder);

// BM_CancelOrder: cost of cancelling a random resting order (id-map lookup
// + intrusive unlink + occasional best-price rescan). Random order matters:
// cancelling sequentially would always hit the same level pattern
void BM_CancelOrder(benchmark::State& state) {
    OrderBook book;
    std::vector<Order>   arena(kBatch);
    std::vector<OrderId> cancel_order_ids(kBatch);
    std::mt19937_64 rng(43);
    OrderId next_id = 1;
    size_t i = kBatch;

    for (auto _ : state) {
        if (i == kBatch) {
            state.PauseTiming();
            regenerate(arena, rng, next_id);
            for (size_t k = 0; k < kBatch; ++k) {
                book.add_order(&arena[k]);
                cancel_order_ids[k] = arena[k].id;
            }
            std::shuffle(cancel_order_ids.begin(), cancel_order_ids.end(), rng);
            i = 0;
            state.ResumeTiming();
        }
        benchmark::DoNotOptimize(book.cancel_order(cancel_order_ids[i++]));
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_CancelOrder);

// BM_MatchOrder: a one-lot bid crossing a resting ask - one fill per
// iteration, nothing rests, so the taker id can be reused. The giant
// resting ask is replenished (off the clock) every ~1e9 fills
void BM_MatchOrder(benchmark::State& state) {
    MatchingEngine eng;
    OrderId maker_id = 1;
    uint64_t maker_remaining = 0;

    NewOrderMessage taker{};
    taker.id       = 0xFFFF'FFFF'FFFF'FFFF;  // never rests, id reuse is safe
    taker.side     = Side::Bid;
    taker.type     = OrderType::Limit;
    taker.price    = kMid;
    taker.quantity = 1;

    for (auto _ : state) {
        if (maker_remaining == 0) {
            state.PauseTiming();
            NewOrderMessage maker{};
            maker.id       = maker_id++;
            maker.side     = Side::Ask;
            maker.type     = OrderType::Limit;
            maker.price    = kMid;
            maker.quantity = 1'000'000'000;
            eng.process_new_order(maker);
            maker_remaining = maker.quantity;
            state.ResumeTiming();
        }
        const auto& fills = eng.process_new_order(taker);
        benchmark::DoNotOptimize(fills.size());
        --maker_remaining;
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_MatchOrder);

// BM_AddCancel_PoolVsMalloc: identical book operations, only the Order
// allocation strategy differs - isolates what the memory pool buys on a
// full add+cancel round trip. Arg(0) = pool, Arg(1) = new/delete
void BM_AddCancel_PoolVsMalloc(benchmark::State& state) {
    const bool use_pool = (state.range(0) == 0);

    OrderBook book;
    MemoryPool<Order, 1024> pool;
    OrderId next_id = 1;

    for (auto _ : state) {
        Order* o = nullptr;
        if (use_pool) {
            o = pool.allocate();
            new (o) Order{};
        } else {
            o = new Order{};
        }
        o->id       = next_id++;
        o->side     = Side::Bid;
        o->type     = OrderType::Limit;
        o->price    = kBidMean;
        o->quantity = 100;

        book.add_order(o);
        book.cancel_order(o->id);

        if (use_pool) {
            o->~Order();
            pool.deallocate(o);
        } else {
            delete o;
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
    state.SetLabel(use_pool ? "pool" : "malloc");
}
BENCHMARK(BM_AddCancel_PoolVsMalloc)->Arg(0)->Arg(1);

// BM_SyntheticStream: millions of generated messages (adds, cancels,
// market orders, natural crossings) through MatchingEngine::process - the
// closest thing to sustained realistic throughput before the live feed
// exists. The engine (book + pool) is rebuilt fresh per iteration, off the
// clock, so every replay starts from an empty book with unique ids
void BM_SyntheticStream(benchmark::State& state) {
    const auto n = static_cast<size_t>(state.range(0));

    synth::WorkloadConfig cfg;
    cfg.cancel_rate = 0.35;  // keeps the resting population well below pool capacity
    const auto messages = synth::WorkloadGenerator(cfg).generate(n);

    std::unique_ptr<MatchingEngine> eng;
    uint64_t fills_per_run = 0;

    for (auto _ : state) {
        state.PauseTiming();
        eng = std::make_unique<MatchingEngine>();  // also frees the previous run
        state.ResumeTiming();

        uint64_t fills = 0;
        for (const auto& msg : messages) {
            fills += eng->process(msg).size();
        }
        benchmark::DoNotOptimize(fills);
        fills_per_run = fills;
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
    state.counters["fills_per_run"] = static_cast<double>(fills_per_run);
}
BENCHMARK(BM_SyntheticStream)
    ->Arg(1 << 20)   // ~1M messages
    ->Arg(1 << 21)   // ~2M messages
    ->Unit(benchmark::kMillisecond);

} // namespace
