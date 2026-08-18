// Unit tests for engine::MatchingEngine
//
// Covers: crossing limit orders, partial fills, price-time priority, market
// sweeps across levels, execution at the passive price, cancels, and pool
// slot accounting (every fully filled or cancelled order returns its slot)

#include <gtest/gtest.h>
#include "core/matching_engine.h"
#include "core/types.h"

namespace {

using namespace engine;

NewOrderMessage limit_order(OrderId id, Side side, Price price, Quantity qty) {
    return NewOrderMessage{id, side, OrderType::Limit, price, qty, 0};
}

NewOrderMessage market_order(OrderId id, Side side, Quantity qty) {
    return NewOrderMessage{id, side, OrderType::Market, 0, qty, 0};
}

class MatchingEngineTest : public ::testing::Test {
protected:
    MatchingEngine engine_;
};

TEST_F(MatchingEngineTest, RestingOrderGeneratesNoFills) {
    const auto& fills = engine_.process_new_order(limit_order(1, Side::Bid, 10'000, 10));
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(engine_.book().best_bid(), 10'000);
    EXPECT_EQ(engine_.book().get_level(10'000).total_quantity(), 10u);
}

TEST_F(MatchingEngineTest, NonCrossingOrdersBothRest) {
    engine_.process_new_order(limit_order(1, Side::Bid, 9'900, 10));
    const auto& fills = engine_.process_new_order(limit_order(2, Side::Ask, 10'100, 10));

    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(engine_.book().best_bid(), 9'900);
    EXPECT_EQ(engine_.book().best_ask(), 10'100);
    EXPECT_EQ(engine_.book().spread(), 200);
}

TEST_F(MatchingEngineTest, LimitOrderCross) {
    engine_.process_new_order(limit_order(1, Side::Bid, 10'000, 10));
    const auto& fills = engine_.process_new_order(limit_order(2, Side::Ask, 10'000, 10));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].aggressive_id, 2u);
    EXPECT_EQ(fills[0].passive_id, 1u);
    EXPECT_EQ(fills[0].price, 10'000);
    EXPECT_EQ(fills[0].quantity, 10u);

    // both orders fully filled - book empty, all pool slots back
    EXPECT_FALSE(engine_.book().has_bids());
    EXPECT_FALSE(engine_.book().has_asks());
    EXPECT_EQ(engine_.pool_available(), MatchingEngine::ORDER_POOL_SIZE);
}

TEST_F(MatchingEngineTest, ExecutionAtPassivePrice) {
    // resting ask at 100.00, aggressive bid willing to pay 100.50 -
    // the trade happens at the passive order's price
    engine_.process_new_order(limit_order(1, Side::Ask, 10'000, 10));
    const auto& fills = engine_.process_new_order(limit_order(2, Side::Bid, 10'050, 10));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].price, 10'000);
}

TEST_F(MatchingEngineTest, PartialFill) {
    engine_.process_new_order(limit_order(1, Side::Bid, 5'000, 100));
    const auto& fills = engine_.process_new_order(limit_order(2, Side::Ask, 5'000, 50));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 50u);
    EXPECT_EQ(fills[0].price, 5'000);

    // ask fully consumed; bid rests with 50 remaining
    EXPECT_FALSE(engine_.book().has_asks());
    EXPECT_EQ(engine_.book().best_bid(), 5'000);
    EXPECT_EQ(engine_.book().get_level(5'000).total_quantity(), 50u);

    Order* resting = engine_.book().get_order(1);
    ASSERT_NE(resting, nullptr);
    EXPECT_EQ(resting->remaining_quantity(), 50u);
}

TEST_F(MatchingEngineTest, AggressorPartialFillRestsRemainder) {
    // incoming bid takes all resting liquidity, remainder rests at its limit
    engine_.process_new_order(limit_order(1, Side::Ask, 10'000, 30));
    const auto& fills = engine_.process_new_order(limit_order(2, Side::Bid, 10'000, 100));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 30u);

    EXPECT_FALSE(engine_.book().has_asks());
    EXPECT_EQ(engine_.book().best_bid(), 10'000);
    EXPECT_EQ(engine_.book().get_level(10'000).total_quantity(), 70u);

    Order* resting = engine_.book().get_order(2);
    ASSERT_NE(resting, nullptr);
    EXPECT_EQ(resting->filled_quantity, 30u);  // partial fill carried over
}

TEST_F(MatchingEngineTest, PriceTimePriority) {
    engine_.process_new_order(limit_order(1, Side::Bid, 10'000, 10));  // first
    engine_.process_new_order(limit_order(2, Side::Bid, 10'000, 10));  // second
    const auto& fills = engine_.process_new_order(limit_order(3, Side::Ask, 10'000, 10));

    // the older order fills first
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].passive_id, 1u);

    EXPECT_EQ(engine_.book().get_order(1), nullptr);   // gone
    EXPECT_NE(engine_.book().get_order(2), nullptr);   // still resting
    EXPECT_EQ(engine_.book().get_level(10'000).total_quantity(), 10u);
}

TEST_F(MatchingEngineTest, BetterPricedLevelFillsFirst) {
    engine_.process_new_order(limit_order(1, Side::Ask, 10'100, 10));
    engine_.process_new_order(limit_order(2, Side::Ask, 10'000, 10));  // better price

    const auto& fills = engine_.process_new_order(limit_order(3, Side::Bid, 10'100, 10));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].passive_id, 2u);   // cheaper ask matched first
    EXPECT_EQ(fills[0].price, 10'000);
    EXPECT_EQ(engine_.book().best_ask(), 10'100);
}

TEST_F(MatchingEngineTest, MarketOrderSweep) {
    engine_.process_new_order(limit_order(1, Side::Ask, 10'000, 5));
    engine_.process_new_order(limit_order(2, Side::Ask, 10'100, 10));
    engine_.process_new_order(limit_order(3, Side::Ask, 10'200, 20));

    const auto& fills = engine_.process_new_order(market_order(4, Side::Bid, 20));

    ASSERT_EQ(fills.size(), 3u);
    EXPECT_EQ(fills[0].quantity, 5u);
    EXPECT_EQ(fills[0].price, 10'000);
    EXPECT_EQ(fills[1].quantity, 10u);
    EXPECT_EQ(fills[1].price, 10'100);
    EXPECT_EQ(fills[2].quantity, 5u);
    EXPECT_EQ(fills[2].price, 10'200);

    // first two levels consumed, third has 15 left
    EXPECT_EQ(engine_.book().best_ask(), 10'200);
    EXPECT_EQ(engine_.book().get_level(10'200).total_quantity(), 15u);
    EXPECT_TRUE(engine_.book().get_level(10'000).empty());
    EXPECT_TRUE(engine_.book().get_level(10'100).empty());
}

TEST_F(MatchingEngineTest, LimitOrderStopsAtItsPrice) {
    engine_.process_new_order(limit_order(1, Side::Ask, 10'000, 10));
    engine_.process_new_order(limit_order(2, Side::Ask, 10'200, 10));

    // bid limit 10'100 crosses the first ask but not the second
    const auto& fills = engine_.process_new_order(limit_order(3, Side::Bid, 10'100, 25));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 10u);

    // remainder (15) rests at the limit price, second ask untouched
    EXPECT_EQ(engine_.book().best_bid(), 10'100);
    EXPECT_EQ(engine_.book().get_level(10'100).total_quantity(), 15u);
    EXPECT_EQ(engine_.book().best_ask(), 10'200);
    EXPECT_EQ(engine_.book().get_level(10'200).total_quantity(), 10u);
}

TEST_F(MatchingEngineTest, MarketOrderRemainderDiscarded) {
    engine_.process_new_order(limit_order(1, Side::Ask, 10'000, 10));

    const auto& fills = engine_.process_new_order(market_order(2, Side::Bid, 50));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 10u);

    // 40 unfilled - discarded, never rests
    EXPECT_FALSE(engine_.book().has_bids());
    EXPECT_FALSE(engine_.book().has_asks());
    EXPECT_EQ(engine_.pool_available(), MatchingEngine::ORDER_POOL_SIZE);
}

TEST_F(MatchingEngineTest, MarketOrderOnEmptyBook) {
    const auto& fills = engine_.process_new_order(market_order(1, Side::Bid, 10));
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(engine_.pool_available(), MatchingEngine::ORDER_POOL_SIZE);
}

TEST_F(MatchingEngineTest, CancelRestingOrder) {
    engine_.process_new_order(limit_order(1, Side::Bid, 9'900, 100));
    EXPECT_EQ(engine_.pool_available(), MatchingEngine::ORDER_POOL_SIZE - 1);

    EXPECT_TRUE(engine_.process_cancel(CancelMessage{1, 0}));

    EXPECT_FALSE(engine_.book().has_bids());
    EXPECT_TRUE(engine_.book().get_level(9'900).empty());
    EXPECT_EQ(engine_.pool_available(), MatchingEngine::ORDER_POOL_SIZE);
}

TEST_F(MatchingEngineTest, CancelUnknownOrderFails) {
    EXPECT_FALSE(engine_.process_cancel(CancelMessage{42, 0}));

    // filled orders are gone - cancelling them fails too
    engine_.process_new_order(limit_order(1, Side::Bid, 10'000, 10));
    engine_.process_new_order(limit_order(2, Side::Ask, 10'000, 10));
    EXPECT_FALSE(engine_.process_cancel(CancelMessage{1, 0}));
}

TEST_F(MatchingEngineTest, VariantDispatch) {
    engine_.process(EngineMessage{limit_order(1, Side::Bid, 10'000, 10)});
    const auto& fills = engine_.process(EngineMessage{limit_order(2, Side::Ask, 10'000, 10)});
    ASSERT_EQ(fills.size(), 1u);

    engine_.process(EngineMessage{limit_order(3, Side::Bid, 9'900, 5)});
    const auto& cancel_fills = engine_.process(EngineMessage{CancelMessage{3, 0}});
    EXPECT_TRUE(cancel_fills.empty());
    EXPECT_FALSE(engine_.book().has_bids());
}

TEST_F(MatchingEngineTest, PoolAccountingAcrossMixedFlow) {
    // 3 resting asks, one big bid sweeps 2.5 of them, cancel the rest
    engine_.process_new_order(limit_order(1, Side::Ask, 10'000, 10));
    engine_.process_new_order(limit_order(2, Side::Ask, 10'100, 10));
    engine_.process_new_order(limit_order(3, Side::Ask, 10'200, 10));
    EXPECT_EQ(engine_.pool_available(), MatchingEngine::ORDER_POOL_SIZE - 3);

    engine_.process_new_order(limit_order(4, Side::Bid, 10'200, 25));  // fully filled, never rests
    EXPECT_EQ(engine_.pool_available(), MatchingEngine::ORDER_POOL_SIZE - 1);  // only order 3 left

    EXPECT_TRUE(engine_.process_cancel(CancelMessage{3, 0}));
    EXPECT_EQ(engine_.pool_available(), MatchingEngine::ORDER_POOL_SIZE);
}

} // namespace
