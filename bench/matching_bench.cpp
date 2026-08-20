// matching_bench.cpp - flat-array engine vs a std::map baseline engine.
//
// This is the benchmark that justifies the project's core design claim:
// that a flat array of price levels indexed by tick beats the "textbook"
// std::map<Price, list<Order>> order book. Both engines process the IDENTICAL
// deterministic synthetic stream (same generator, same seed, same config as
// BM_SyntheticStream in order_book_bench), so the throughput difference is
// purely the data-structure choice: array indexing + memory pool + intrusive
// lists versus red-black tree traversal + node allocation on every insert.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

#include "core/matching_engine.h"
#include "core/types.h"
#include "tools/synthetic_workload.h"
#include "transport/message.h"

namespace {

using namespace engine;

// ---------------------------------------------------------------------------
// The baseline: how an order book looks before latency engineering.
//   - price levels in std::map (red-black tree: O(log N) lookup, pointer
//     chasing on every level walk, one heap node per level)
//   - orders in std::list per level (one heap allocation per resting order)
//   - id lookup via std::unordered_map, storing list iterators
// Matching semantics mirror MatchingEngine: price-time priority, limit
// remainders rest, market remainders are discarded.
// ---------------------------------------------------------------------------
class MapBookEngine {
public:
    uint64_t process(const EngineMessage& msg) {
        return std::visit([this](const auto& m) { return handle(m); }, msg);
    }

private:
    struct RestingOrder {
        OrderId  id  = 0;
        Quantity qty = 0;
    };
    using Level = std::list<RestingOrder>;

    // bids keyed descending so begin() is always the best price on both maps
    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level>                      asks_;

    struct Location {
        Side            side;
        Price           price;
        Level::iterator it;
    };
    std::unordered_map<OrderId, Location> index_;

    uint64_t handle(const NewOrderMessage& m) {
        Quantity remaining = m.quantity;
        const bool is_market = (m.type == OrderType::Market);
        uint64_t fills = 0;

        if (m.side == Side::Bid) {
            fills = match(asks_, remaining, [&](Price lvl) {
                return is_market || lvl <= m.price;
            });
        } else {
            fills = match(bids_, remaining, [&](Price lvl) {
                return is_market || lvl >= m.price;
            });
        }

        if (remaining > 0 && !is_market) {
            auto& level = (m.side == Side::Bid) ? bids_[m.price] : asks_[m.price];
            level.push_back(RestingOrder{m.id, remaining});
            index_.emplace(m.id,
                           Location{m.side, m.price, std::prev(level.end())});
        }
        return fills;
    }

    uint64_t handle(const CancelMessage& m) {
        const auto it = index_.find(m.id);
        if (it == index_.end()) return 0;
        const Location& loc = it->second;
        if (loc.side == Side::Bid) {
            erase_from(bids_, loc);
        } else {
            erase_from(asks_, loc);
        }
        index_.erase(it);
        return 0;
    }

    template <typename Book, typename Crosses>
    uint64_t match(Book& book, Quantity& remaining, Crosses crosses) {
        uint64_t fills = 0;
        while (remaining > 0 && !book.empty()) {
            const auto lvl = book.begin();  // best price by map ordering
            if (!crosses(lvl->first)) break;
            Level& queue = lvl->second;
            while (remaining > 0 && !queue.empty()) {
                RestingOrder& top = queue.front();
                const Quantity executed = std::min(remaining, top.qty);
                top.qty  = static_cast<Quantity>(top.qty - executed);
                remaining = static_cast<Quantity>(remaining - executed);
                ++fills;
                if (top.qty == 0) {
                    index_.erase(top.id);
                    queue.pop_front();
                }
            }
            if (queue.empty()) book.erase(lvl);
        }
        return fills;
    }

    template <typename Book>
    void erase_from(Book& book, const Location& loc) {
        const auto lvl = book.find(loc.price);
        lvl->second.erase(loc.it);
        if (lvl->second.empty()) book.erase(lvl);
    }
};

// identical workload to BM_SyntheticStream in order_book_bench
std::vector<EngineMessage> make_stream(size_t n) {
    synth::WorkloadConfig cfg;
    cfg.cancel_rate = 0.35;
    return synth::WorkloadGenerator(cfg).generate(n);
}

// ---------------------------------------------------------------------------
// The two engines, same stream, same measurement shape
// ---------------------------------------------------------------------------

void BM_Stream_FlatBook(benchmark::State& state) {
    const auto n = static_cast<size_t>(state.range(0));
    const auto messages = make_stream(n);

    std::unique_ptr<MatchingEngine> eng;
    uint64_t fills_per_run = 0;

    for (auto _ : state) {
        state.PauseTiming();
        eng = std::make_unique<MatchingEngine>();
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
BENCHMARK(BM_Stream_FlatBook)->Arg(1 << 20)->Unit(benchmark::kMillisecond);

void BM_Stream_StdMapBook(benchmark::State& state) {
    const auto n = static_cast<size_t>(state.range(0));
    const auto messages = make_stream(n);

    std::unique_ptr<MapBookEngine> eng;
    uint64_t fills_per_run = 0;

    for (auto _ : state) {
        state.PauseTiming();
        eng = std::make_unique<MapBookEngine>();
        state.ResumeTiming();

        uint64_t fills = 0;
        for (const auto& msg : messages) {
            fills += eng->process(msg);
        }
        benchmark::DoNotOptimize(fills);
        fills_per_run = fills;
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
    state.counters["fills_per_run"] = static_cast<double>(fills_per_run);
}
BENCHMARK(BM_Stream_StdMapBook)->Arg(1 << 20)->Unit(benchmark::kMillisecond);

} // namespace
