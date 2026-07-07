// matching_engine_test.cpp — Unit tests for engine::MatchingEngine
// Built from top-level CMakeLists.txt as part of the unit_tests target.

#include <gtest/gtest.h>
#include "core/matching_engine.h"
#include "core/order_book.h"
#include "core/types.h"

// ---------------------------------------------------------------------------
// Tests — all TODO stubs, to be implemented once MatchingEngine API settles
// ---------------------------------------------------------------------------

TEST(MatchingEngineTest, LimitOrderCross) {
    // TODO: Create MatchingEngine for symbol "BTC-USD".
    //       Submit a Bid limit order at price 100.00, qty 10.
    //       Submit an Ask limit order at price 100.00, qty 10.
    //       Verify:
    //         - A fill / trade event is generated.
    //         - Fill price == 100.00, fill qty == 10.
    //         - Both orders are fully filled (removed from the book).
    //         - The book is empty on both sides after matching.
}

TEST(MatchingEngineTest, PartialFill) {
    // TODO: Submit a Bid limit order at price 50.00, qty 100.
    //       Submit an Ask limit order at price 50.00, qty 50.
    //       Verify:
    //         - A fill event is generated for qty 50 at price 50.00.
    //         - The Ask order is fully consumed.
    //         - The Bid order rests with remaining qty 50.
    //         - best_bid() == 50.00 with level qty == 50.
}

TEST(MatchingEngineTest, PriceTimePriority) {
    // TODO: Submit Bid order A at price 100.00, qty 10  (arrives first).
    //       Submit Bid order B at price 100.00, qty 10  (arrives second).
    //       Submit an Ask order at price 100.00, qty 10.
    //       Verify:
    //         - Order A is filled first (FIFO at same price level).
    //         - Order B still rests in the book.
    //         - The fill event references order A's ID, not B's.
}

TEST(MatchingEngineTest, MarketOrderSweep) {
    // TODO: Build an ask book:
    //         Level 100.00: qty 5
    //         Level 101.00: qty 10
    //         Level 102.00: qty 20
    //       Submit a Market Bid order with qty 20.
    //       Verify:
    //         - Fills across multiple levels: 5@100, 10@101, 5@102.
    //         - Level 100.00 and 101.00 are fully consumed.
    //         - Level 102.00 has remaining qty 15.
    //         - best_ask() == 102.00.
}

TEST(MatchingEngineTest, CancelReducesQuantity) {
    // TODO: Submit a Bid limit order at price 99.00, qty 100.
    //       Cancel the order.
    //       Verify:
    //         - The order is removed from the book.
    //         - The level at 99.00 has qty 0 (or is removed).
    //         - best_bid() is updated (sentinel if book is empty).
}
