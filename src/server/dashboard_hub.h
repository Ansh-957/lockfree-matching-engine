#pragma once

// Shared snapshot state between the pipeline threads and the dashboard
// WebSocket server. Three writers, one reader, all OFF the hot path:
//
//   engine thread  - publishes top-of-book levels, but only when the server
//                    has raised the book_wanted_ flag (10Hz), and only via
//                    try_lock: the matching loop can never block behind the
//                    JSON serializer. A missed snapshot costs one frame
//   output thread  - publishes fill rows + latency/throughput stats at its
//                    own cadence (~100ms), batched under one lock
//   server thread  - serializes everything to JSON at broadcast time
//
// The hub has no Boost dependency so it builds on every platform; only the
// server (ws_server.h) needs the feed toolchain

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "core/order_book.h"
#include "core/types.h"
#include "transport/message.h"

namespace engine {

class DashboardHub {
public:
    static constexpr size_t BOOK_DEPTH = 15;   // levels per side in the UI
    static constexpr size_t FILL_KEEP  = 48;   // tape rows kept server-side

    // bounded best-price walk: don't scan further than this many ticks from
    // the best when collecting display levels (sparse synthetic books)
    static constexpr Price SCAN_LIMIT = 100'000;

    struct Level {
        Price    price  = 0;
        uint64_t qty    = 0;
        uint32_t orders = 0;
    };

    struct FillRow {
        uint64_t seq          = 0;  // monotone id so the UI can dedupe frames
        uint64_t ts_ms        = 0;  // wall clock at the output thread
        Price    price        = 0;
        Quantity qty          = 0;
        bool     taker_is_bid = false;
    };

    struct Stats {
        uint64_t match_p50 = 0, match_p95 = 0, match_p99 = 0;
        uint64_t order_p50 = 0, order_p99 = 0;
        uint64_t processed = 0, fills = 0;
        uint64_t pool_free = 0, pool_capacity = 0;
        uint64_t dropped   = 0;
        uint64_t clients   = 0;
    };

    void configure(std::string mode, std::string product) {
        std::lock_guard lk(mu_);
        mode_    = std::move(mode);
        product_ = std::move(product);
    }

    // server thread: raise the flag; the engine services it on its next
    // loop iteration (a relaxed bool load per iteration, ~free)
    void request_book() { book_wanted_.store(true, std::memory_order_relaxed); }

    [[nodiscard]] bool book_wanted() const {
        return book_wanted_.load(std::memory_order_relaxed);
    }

    // engine thread only
    void publish_book(const OrderBook& book) {
        // try_lock: if the serializer holds the lock right now, skip this
        // frame rather than stall the matching loop
        std::unique_lock lk(mu_, std::try_to_lock);
        if (!lk.owns_lock()) {
            return;  // flag stays up; retry next iteration
        }
        book_wanted_.store(false, std::memory_order_relaxed);

        bid_count_ = 0;
        if (book.has_bids()) {
            const Price best  = book.best_bid();
            const Price floor = (best > SCAN_LIMIT) ? best - SCAN_LIMIT : 0;
            for (Price p = best; p >= floor && bid_count_ < BOOK_DEPTH; --p) {
                const PriceLevel& lvl = book.get_level(p);
                if (!lvl.empty()) {
                    bids_[bid_count_++] = {p, lvl.total_quantity(), lvl.order_count()};
                }
            }
        }

        ask_count_ = 0;
        if (book.has_asks()) {
            const Price best = book.best_ask();
            const Price cap  = (best < MAX_PRICE_TICKS - SCAN_LIMIT)
                                 ? best + SCAN_LIMIT : MAX_PRICE_TICKS - 1;
            for (Price p = best; p <= cap && ask_count_ < BOOK_DEPTH; ++p) {
                const PriceLevel& lvl = book.get_level(p);
                if (!lvl.empty()) {
                    asks_[ask_count_++] = {p, lvl.total_quantity(), lvl.order_count()};
                }
            }
        }

        total_bids_ = book.bid_count();
        total_asks_ = book.ask_count();
    }

    // output thread: batched fills + stats, one lock per cadence tick
    void publish_output(const Stats& stats, const FillRow* rows, size_t n) {
        std::lock_guard lk(mu_);
        stats_ = stats;
        for (size_t i = 0; i < n; ++i) {
            fills_[fill_head_] = rows[i];
            fill_head_ = (fill_head_ + 1) % FILL_KEEP;
            if (fill_count_ < FILL_KEEP) ++fill_count_;
        }
    }

    // server thread: one JSON frame with everything the UI needs.
    // Hand-rolled: the payload is small, the schema is fixed, and it saves
    // a JSON library dependency for a ~3KB string built at 10Hz
    [[nodiscard]] std::string snapshot_json(uint64_t now_ms) {
        std::lock_guard lk(mu_);

        std::string out;
        out.reserve(4096);
        char buf[160];

        std::snprintf(buf, sizeof(buf),
            R"({"t":%llu,"mode":"%s","product":"%s","bids":[)",
            static_cast<unsigned long long>(now_ms),
            mode_.c_str(), product_.c_str());
        out += buf;

        for (size_t i = 0; i < bid_count_; ++i) {
            std::snprintf(buf, sizeof(buf), "%s[%lld,%llu,%u]", i ? "," : "",
                static_cast<long long>(bids_[i].price),
                static_cast<unsigned long long>(bids_[i].qty),
                bids_[i].orders);
            out += buf;
        }
        out += R"(],"asks":[)";
        for (size_t i = 0; i < ask_count_; ++i) {
            std::snprintf(buf, sizeof(buf), "%s[%lld,%llu,%u]", i ? "," : "",
                static_cast<long long>(asks_[i].price),
                static_cast<unsigned long long>(asks_[i].qty),
                asks_[i].orders);
            out += buf;
        }

        // oldest-first so the UI can append in arrival order
        out += R"(],"fills":[)";
        for (size_t i = 0; i < fill_count_; ++i) {
            const size_t idx = (fill_head_ + FILL_KEEP - fill_count_ + i) % FILL_KEEP;
            const FillRow& f = fills_[idx];
            std::snprintf(buf, sizeof(buf), "%s[%llu,%llu,%lld,%u,%d]", i ? "," : "",
                static_cast<unsigned long long>(f.seq),
                static_cast<unsigned long long>(f.ts_ms),
                static_cast<long long>(f.price),
                f.qty, f.taker_is_bid ? 1 : 0);
            out += buf;
        }

        std::snprintf(buf, sizeof(buf),
            R"(],"stats":{"match_p50":%llu,"match_p95":%llu,"match_p99":%llu,)"
            R"("order_p50":%llu,"order_p99":%llu,)",
            static_cast<unsigned long long>(stats_.match_p50),
            static_cast<unsigned long long>(stats_.match_p95),
            static_cast<unsigned long long>(stats_.match_p99),
            static_cast<unsigned long long>(stats_.order_p50),
            static_cast<unsigned long long>(stats_.order_p99));
        out += buf;

        std::snprintf(buf, sizeof(buf),
            R"("processed":%llu,"fills":%llu,"pool_free":%llu,"pool_capacity":%llu,)"
            R"("dropped":%llu,"clients":%llu,"book_bids":%llu,"book_asks":%llu}})",
            static_cast<unsigned long long>(stats_.processed),
            static_cast<unsigned long long>(stats_.fills),
            static_cast<unsigned long long>(stats_.pool_free),
            static_cast<unsigned long long>(stats_.pool_capacity),
            static_cast<unsigned long long>(stats_.dropped),
            static_cast<unsigned long long>(stats_.clients),
            static_cast<unsigned long long>(total_bids_),
            static_cast<unsigned long long>(total_asks_));
        out += buf;

        return out;
    }

private:
    std::atomic<bool> book_wanted_{false};

    std::mutex mu_;
    std::string mode_    = "synthetic";
    std::string product_ = "SYNTH";

    std::array<Level, BOOK_DEPTH> bids_{};
    std::array<Level, BOOK_DEPTH> asks_{};
    size_t   bid_count_  = 0;
    size_t   ask_count_  = 0;
    uint64_t total_bids_ = 0;
    uint64_t total_asks_ = 0;

    std::array<FillRow, FILL_KEEP> fills_{};
    size_t fill_head_  = 0;  // next write position
    size_t fill_count_ = 0;

    Stats stats_{};
};

} // namespace engine
