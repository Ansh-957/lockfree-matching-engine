#include "feed/feed_handler.h"

#include <simdjson.h>

#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] Timestamp now_ns() noexcept {
    return static_cast<Timestamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] bool parse_double(std::string_view s, double& out) noexcept {
    if (s.empty()) {
        return false;
    }
    const char* begin = s.data();
    const char* end   = begin + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

[[nodiscard]] bool to_ticks(std::string_view s, Price& out, bool& out_of_range) noexcept {
    out_of_range = false;
    double dollars = 0.0;
    if (!parse_double(s, dollars)) {
        return false;
    }
    const Price p = dollars_to_ticks(dollars);
    if (p <= 0 || p >= MAX_PRICE_TICKS) {
        out_of_range = true;
        return false;
    }
    out = p;
    return true;
}

[[nodiscard]] bool to_lots(std::string_view s, Quantity& out) noexcept {
    double coins = 0.0;
    if (!parse_double(s, coins)) {
        return false;
    }
    if (coins <= 0.0) {
        out = 0;
        return true;
    }
    const double scaled = coins * LOTS_PER_COIN;
    if (scaled >= static_cast<double>(std::numeric_limits<Quantity>::max())) {
        out = std::numeric_limits<Quantity>::max();
        return true;
    }
    const auto rounded = std::llround(scaled);
    out = rounded < 0 ? 0 : static_cast<Quantity>(rounded);
    return true;
}

[[nodiscard]] bool parse_side(std::string_view s, Side& out) noexcept {
    if (s == "buy" || s == "bid") {
        out = Side::Bid;
        return true;
    }
    if (s == "sell" || s == "ask") {
        out = Side::Ask;
        return true;
    }
    return false;
}

[[nodiscard]] bool json_string(simdjson::dom::element el, std::string_view& out) {
    return !el.get_string().get(out);
}

struct LevelKey {
    Price price = 0;
    Side  side  = Side::Bid;

    bool operator==(const LevelKey& o) const noexcept {
        return price == o.price && side == o.side;
    }
};

struct LevelKeyHash {
    std::size_t operator()(const LevelKey& k) const noexcept {
        return static_cast<std::size_t>(k.price) * 2u
             + static_cast<std::size_t>(k.side);
    }
};

} // namespace

struct FeedHandler::Impl {
    simdjson::dom::parser parser;
    std::string padded;

    std::unordered_map<LevelKey, OrderId, LevelKeyHash> live;
    OrderId next_id = 1;

    uint64_t last_sequence = 0;
    bool     have_sequence = false;
    uint64_t malformed     = 0;
    uint64_t sequence_gaps = 0;
    uint64_t ignored       = 0;
    uint64_t skipped       = 0;

    void note_sequence(uint64_t seq) {
        if (have_sequence && seq != last_sequence + 1) {
            ++sequence_gaps;
        }
        have_sequence = true;
        last_sequence = seq;
    }

    void apply_level(Side side, Price price, Quantity qty, Timestamp ts,
                     std::vector<EngineMessage>& out) {
        const LevelKey key{price, side};
        if (const auto it = live.find(key); it != live.end()) {
            out.emplace_back(CancelMessage{it->second, ts});
            live.erase(it);
        }
        if (qty == 0) {
            return;
        }
        const OrderId id = next_id++;
        live.emplace(key, id);
        out.emplace_back(NewOrderMessage{
            id, side, OrderType::Limit, price, qty, ts});
    }

    void reset_book(Timestamp ts, std::vector<EngineMessage>& out) {
        for (const auto& [key, id] : live) {
            (void)key;
            out.emplace_back(CancelMessage{id, ts});
        }
        live.clear();
    }

    void consume_price_size_array(simdjson::dom::array levels, Side side,
                                  Timestamp ts, std::vector<EngineMessage>& out,
                                  bool skip_zero) {
        for (auto row_el : levels) {
            simdjson::dom::array row;
            if (row_el.get_array().get(row)) {
                ++malformed;
                continue;
            }
            std::string_view price_s;
            std::string_view size_s;
            int i = 0;
            for (auto el : row) {
                std::string_view s;
                if (!json_string(el, s)) {
                    i = -1;
                    break;
                }
                if (i == 0)      price_s = s;
                else if (i == 1) size_s  = s;
                ++i;
            }
            if (i < 2) {
                ++malformed;
                continue;
            }
            Price price = 0;
            Quantity qty = 0;
            bool oob = false;
            if (!to_ticks(price_s, price, oob) || !to_lots(size_s, qty)) {
                if (oob) {
                    ++skipped;
                } else {
                    ++malformed;
                }
                continue;
            }
            if (skip_zero && qty == 0) {
                continue;
            }
            apply_level(side, price, qty, ts, out);
        }
    }
};

FeedHandler::FeedHandler() : impl_(std::make_unique<Impl>()) {}
FeedHandler::~FeedHandler() = default;

std::size_t FeedHandler::parse(std::string_view raw_json,
                               std::vector<EngineMessage>& out) {
    const std::size_t before = out.size();

    // simdjson reads up to SIMDJSON_PADDING bytes past the JSON. The
    // WebSocket buffer has no such padding, so we copy into a reusable
    // string that does. After the first snapshot the allocation sticks.
    impl_->padded.assign(raw_json.data(), raw_json.size());
    impl_->padded.append(simdjson::SIMDJSON_PADDING, '\0');

    simdjson::dom::element doc;
    if (impl_->parser.parse(impl_->padded.data(), raw_json.size(), false).get(doc)) {
        ++impl_->malformed;
        return 0;
    }

    std::string_view type;
    if (doc["type"].get(type)) {
        ++impl_->malformed;
        return 0;
    }

    const Timestamp ts = now_ns();

    if (type == "l2update") {
        simdjson::dom::array changes;
        if (doc["changes"].get(changes)) {
            ++impl_->malformed;
            return 0;
        }
        for (auto change : changes) {
            simdjson::dom::array row;
            if (change.get_array().get(row)) {
                ++impl_->malformed;
                continue;
            }
            std::string_view side_s;
            std::string_view price_s;
            std::string_view size_s;
            int field = 0;
            for (auto el : row) {
                std::string_view s;
                if (!json_string(el, s)) {
                    field = -1;
                    break;
                }
                if (field == 0)      side_s  = s;
                else if (field == 1) price_s = s;
                else if (field == 2) size_s  = s;
                ++field;
            }
            if (field < 3) {
                ++impl_->malformed;
                continue;
            }
            Side side{};
            Price price = 0;
            Quantity qty = 0;
            bool oob = false;
            if (!parse_side(side_s, side) || !to_ticks(price_s, price, oob)
                || !to_lots(size_s, qty)) {
                if (oob) {
                    ++impl_->skipped;
                } else {
                    ++impl_->malformed;
                }
                continue;
            }
            impl_->apply_level(side, price, qty, ts, out);
        }
        return out.size() - before;
    }

    if (type == "snapshot") {
        impl_->reset_book(ts, out);
        simdjson::dom::array bids;
        simdjson::dom::array asks;
        if (!doc["bids"].get(bids)) {
            impl_->consume_price_size_array(bids, Side::Bid, ts, out, true);
        }
        if (!doc["asks"].get(asks)) {
            impl_->consume_price_size_array(asks, Side::Ask, ts, out, true);
        }
        return out.size() - before;
    }

    if (type == "match" || type == "last_match") {
        // A match already happened on Coinbase. Our book is driven by L2
        // quantity changes, so injecting a synthetic aggressor here would
        // double-count. Matches DO carry the per-product sequence number
        // (heartbeats also have a "sequence" but it is a different series,
        // so we only track it here).
        uint64_t seq = 0;
        if (!doc["sequence"].get(seq)) {
            impl_->note_sequence(seq);
        }
        ++impl_->ignored;
        return 0;
    }

    ++impl_->ignored;
    return 0;
}

uint64_t FeedHandler::malformed_count() const noexcept { return impl_->malformed; }
uint64_t FeedHandler::sequence_gaps()   const noexcept { return impl_->sequence_gaps; }
uint64_t FeedHandler::ignored_count()   const noexcept { return impl_->ignored; }
uint64_t FeedHandler::skipped_count()   const noexcept { return impl_->skipped; }
std::size_t FeedHandler::live_levels()  const noexcept { return impl_->live.size(); }

} // namespace engine
