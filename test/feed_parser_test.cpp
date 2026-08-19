#include <gtest/gtest.h>

#include "core/types.h"
#include "feed/feed_handler.h"
#include "transport/message.h"

#include <cmath>
#include <string_view>
#include <variant>
#include <vector>

using engine::CancelMessage;
using engine::EngineMessage;
using engine::FeedHandler;
using engine::NewOrderMessage;
using engine::OrderType;
using engine::Side;
using engine::dollars_to_ticks;
using engine::LOTS_PER_COIN;

namespace {

[[nodiscard]] const NewOrderMessage* as_new(const EngineMessage& m) {
    return std::get_if<NewOrderMessage>(&m);
}

[[nodiscard]] const CancelMessage* as_cancel(const EngineMessage& m) {
    return std::get_if<CancelMessage>(&m);
}

[[nodiscard]] engine::Quantity lots(double coins) {
    return static_cast<engine::Quantity>(std::llround(coins * LOTS_PER_COIN));
}

} // namespace

class FeedParserTest : public ::testing::Test {
protected:
    FeedHandler handler;
    std::vector<EngineMessage> out;
};

TEST_F(FeedParserTest, ParseCoinbaseL2Update) {
    const char* json =
        R"({"type":"l2update","product_id":"BTC-USD",)"
        R"("time":"2024-01-01T00:00:00.000000Z",)"
        R"("changes":[["buy","50000.00","1.5"]]})";

    ASSERT_EQ(handler.parse(json, out), 1u);
    const auto* o = as_new(out[0]);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->side, Side::Bid);
    EXPECT_EQ(o->type, OrderType::Limit);
    EXPECT_EQ(o->price, dollars_to_ticks(50000.00));
    EXPECT_EQ(o->quantity, lots(1.5));
    EXPECT_NE(o->id, 0u);
    EXPECT_NE(o->timestamp, 0u);
    EXPECT_EQ(handler.live_levels(), 1u);
}

TEST_F(FeedParserTest, L2UpdateZeroSizeCancels) {
    ASSERT_EQ(handler.parse(
        R"({"type":"l2update","changes":[["sell","101.25","2.0"]]})", out), 1u);
    const auto* added = as_new(out[0]);
    ASSERT_NE(added, nullptr);
    const auto id = added->id;

    out.clear();
    ASSERT_EQ(handler.parse(
        R"({"type":"l2update","changes":[["sell","101.25","0"]]})", out), 1u);
    const auto* c = as_cancel(out[0]);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->id, id);
    EXPECT_EQ(handler.live_levels(), 0u);
}

TEST_F(FeedParserTest, ReplaceIsCancelThenNew) {
    ASSERT_EQ(handler.parse(
        R"({"type":"l2update","changes":[["buy","100.00","1.0"]]})", out), 1u);
    const auto first_id = as_new(out[0])->id;

    out.clear();
    ASSERT_EQ(handler.parse(
        R"({"type":"l2update","changes":[["buy","100.00","3.0"]]})", out), 2u);
    const auto* c = as_cancel(out[0]);
    const auto* n = as_new(out[1]);
    ASSERT_NE(c, nullptr);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(c->id, first_id);
    EXPECT_NE(n->id, first_id);
    EXPECT_EQ(n->quantity, lots(3.0));
    EXPECT_EQ(handler.live_levels(), 1u);
}

TEST_F(FeedParserTest, SnapshotEmitsOneOrderPerLevel) {
    const char* json =
        R"({"type":"snapshot","product_id":"BTC-USD",)"
        R"("bids":[["99.00","1.0"],["98.50","2.0"]],)"
        R"("asks":[["100.00","1.5"]]})";

    ASSERT_EQ(handler.parse(json, out), 3u);
    EXPECT_EQ(as_new(out[0])->side, Side::Bid);
    EXPECT_EQ(as_new(out[0])->price, dollars_to_ticks(99.00));
    EXPECT_EQ(as_new(out[1])->side, Side::Bid);
    EXPECT_EQ(as_new(out[1])->price, dollars_to_ticks(98.50));
    EXPECT_EQ(as_new(out[2])->side, Side::Ask);
    EXPECT_EQ(as_new(out[2])->price, dollars_to_ticks(100.00));
    EXPECT_EQ(as_new(out[2])->quantity, lots(1.5));
    EXPECT_EQ(handler.live_levels(), 3u);
}

TEST_F(FeedParserTest, SnapshotResetsExistingBook) {
    handler.parse(R"({"type":"l2update","changes":[["buy","50.00","1.0"]]})", out);
    const auto old_id = as_new(out[0])->id;
    out.clear();

    handler.parse(R"({"type":"snapshot","bids":[["51.00","1.0"]],"asks":[]})", out);
    ASSERT_GE(out.size(), 2u);
    const auto* c = as_cancel(out[0]);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->id, old_id);
    const auto* n = as_new(out.back());
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->price, dollars_to_ticks(51.00));
    EXPECT_EQ(handler.live_levels(), 1u);
}

TEST_F(FeedParserTest, MultipleChangesInOneFrame) {
    const char* json =
        R"({"type":"l2update","changes":[)"
        R"(["buy","100.00","1.0"],["sell","101.00","2.0"],["buy","99.00","0.5"]]})";
    ASSERT_EQ(handler.parse(json, out), 3u);
    EXPECT_EQ(handler.live_levels(), 3u);
}

TEST_F(FeedParserTest, HandleMalformedJson) {
    const char* junk[] = {
        "",
        "{",
        "null",
        "[]",
        "not json",
        R"({"type":"l2update"})",                 // missing changes
        R"({"type":"l2update","changes":"nope"})",
        R"({"type":"l2update","changes":[["buy","abc","1"]]})",  // bad price
    };
    for (const char* j : junk) {
        out.clear();
        EXPECT_EQ(handler.parse(j, out), 0u) << j;
        EXPECT_TRUE(out.empty()) << j;
    }
    EXPECT_GT(handler.malformed_count(), 0u);
    EXPECT_EQ(handler.live_levels(), 0u);
}

TEST_F(FeedParserTest, SequenceGapDetection) {
    handler.parse(R"({"type":"match","sequence":10,"price":"100.00","size":"1"})", out);
    EXPECT_EQ(handler.sequence_gaps(), 0u);
    EXPECT_TRUE(out.empty());

    handler.parse(R"({"type":"match","sequence":12,"price":"100.00","size":"1"})", out);
    EXPECT_EQ(handler.sequence_gaps(), 1u);  // expected 11, got 12

    handler.parse(R"({"type":"match","sequence":13,"price":"100.00","size":"1"})", out);
    EXPECT_EQ(handler.sequence_gaps(), 1u);  // 12 -> 13 is contiguous
}

TEST_F(FeedParserTest, DuplicateSequenceIsAGap) {
    handler.parse(R"({"type":"match","sequence":5})", out);
    handler.parse(R"({"type":"match","sequence":5})", out);
    EXPECT_EQ(handler.sequence_gaps(), 1u);
}

TEST_F(FeedParserTest, IgnoreHeartbeatAndSubscriptions) {
    EXPECT_EQ(handler.parse(R"({"type":"heartbeat","on_time":true})", out), 0u);
    EXPECT_EQ(handler.parse(
        R"({"type":"subscriptions","channels":[{"name":"level2"}]})", out), 0u);
    EXPECT_EQ(handler.ignored_count(), 2u);
    EXPECT_EQ(handler.malformed_count(), 0u);
}

TEST_F(FeedParserTest, OutOfRangePriceIsSkipped) {
    // $200,000 is past MAX_PRICE_TICKS ($100,000)
    EXPECT_EQ(handler.parse(
        R"({"type":"l2update","changes":[["buy","200000.00","1.0"]]})", out), 0u);
    EXPECT_EQ(handler.live_levels(), 0u);
    EXPECT_EQ(handler.malformed_count(), 0u);
    EXPECT_EQ(handler.skipped_count(), 1u);
}

TEST_F(FeedParserTest, MatchDoesNotCreateOrders) {
    EXPECT_EQ(handler.parse(
        R"({"type":"match","sequence":1,"side":"buy","price":"100.00","size":"1.0"})",
        out), 0u);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(handler.live_levels(), 0u);
}
