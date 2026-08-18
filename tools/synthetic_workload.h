#pragma once

// Deterministic synthetic order-flow generator, shared by the CLI tool
// (synthetic_generator.cpp) and the benchmarks
//
// Produces a stream of NewOrderMessage / CancelMessage that resembles real
// flow: limit prices drawn from a normal distribution around a mid price,
// a configurable fraction of market orders, and cancels that always target
// a randomly chosen still-live order id (so the engine never sees a cancel
// for an id that was never created)
//
// Deterministic by construction: fixed RNG seed, and timestamps are a
// logical counter instead of wall-clock reads - two runs with the same
// config produce identical streams, which makes benchmark results
// comparable across runs and machines

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "core/types.h"
#include "transport/message.h"

namespace engine::synth {

struct WorkloadConfig {
    double   cancel_rate          = 0.20;   // fraction of messages that cancel a live order
    double   market_rate          = 0.10;   // fraction of new orders that are market
    double   price_mean_dollars   = 100.0;
    double   price_stddev_dollars = 2.0;
    Quantity min_quantity         = 1;
    Quantity max_quantity         = 500;
    uint64_t seed                 = 42;
};

class WorkloadGenerator {
public:
    explicit WorkloadGenerator(const WorkloadConfig& cfg = {})
        : cfg_(cfg),
          rng_(cfg.seed),
          price_dist_(cfg.price_mean_dollars, cfg.price_stddev_dollars),
          coin_(0.0, 1.0),
          qty_dist_(cfg.min_quantity, cfg.max_quantity) {}

    // next message in the stream
    [[nodiscard]] EngineMessage next() {
        const bool do_cancel = !live_ids_.empty() && coin_(rng_) < cfg_.cancel_rate;
        return do_cancel ? EngineMessage{make_cancel()}
                         : EngineMessage{make_new_order()};
    }

    [[nodiscard]] std::vector<EngineMessage> generate(std::size_t n) {
        std::vector<EngineMessage> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(next());
        }
        return out;
    }

    // ids the generator believes are still resting (the engine may have
    // filled some of them - a cancel for a filled id is a realistic no-op)
    [[nodiscard]] std::size_t live_order_count() const noexcept { return live_ids_.size(); }
    [[nodiscard]] OrderId     orders_created()  const noexcept { return next_id_ - 1; }

private:
    [[nodiscard]] NewOrderMessage make_new_order() {
        NewOrderMessage msg{};
        msg.id   = next_id_++;
        msg.side = (coin_(rng_) < 0.5) ? Side::Bid : Side::Ask;
        msg.type = (coin_(rng_) < cfg_.market_rate) ? OrderType::Market
                                                    : OrderType::Limit;

        const double dollars = std::max(DOLLARS_PER_TICK, price_dist_(rng_));
        msg.price = std::clamp<Price>(dollars_to_ticks(dollars), 1, MAX_PRICE_TICKS - 1);

        msg.quantity  = qty_dist_(rng_);
        msg.timestamp = ++logical_clock_;

        // only limit orders can rest, so only they are cancel candidates
        if (msg.type == OrderType::Limit) {
            live_ids_.push_back(msg.id);
        }
        return msg;
    }

    [[nodiscard]] CancelMessage make_cancel() {
        std::uniform_int_distribution<std::size_t> idx_dist(0, live_ids_.size() - 1);
        const std::size_t idx = idx_dist(rng_);

        CancelMessage msg{};
        msg.id        = live_ids_[idx];
        msg.timestamp = ++logical_clock_;

        // swap-and-pop: O(1) removal, order of the live set doesn't matter
        std::swap(live_ids_[idx], live_ids_.back());
        live_ids_.pop_back();
        return msg;
    }

    WorkloadConfig cfg_;
    std::mt19937_64 rng_;
    std::normal_distribution<double>       price_dist_;
    std::uniform_real_distribution<double> coin_;
    std::uniform_int_distribution<Quantity> qty_dist_;

    std::vector<OrderId> live_ids_;
    OrderId   next_id_       = 1;
    Timestamp logical_clock_ = 0;
};

} // namespace engine::synth
