// order_book_test.cpp — Unit tests for engine::OrderBook
// Built from top-level CMakeLists.txt as part of the unit_tests target.

#include <gtest/gtest.h>
#include "core/order_book.h"
#include "core/types.h"

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(OrderBookTest, EmptyBook) {
    // TODO: Construct an empty OrderBook.
    //       Verify best_bid() and best_ask() return sentinel / invalid values.
    //       Verify the book reports no levels on either side.
}

TEST(OrderBookTest, AddSingleBidOrder) {
    // TODO: Construct OrderBook, add one Bid limit order at price 100.00.
    //       Verify best_bid() == 100.00.
    //       Verify the bid level at 100.00 has quantity == order.qty.
}

TEST(OrderBookTest, AddSingleAskOrder) {
    // TODO: Construct OrderBook, add one Ask limit order at price 101.50.
    //       Verify best_ask() == 101.50.
    //       Verify the ask level at 101.50 has quantity == order.qty.
}

TEST(OrderBookTest, CancelOrder) {
    // TODO: Add a Bid order, then cancel it by order_id.
    //       Verify the level is now empty (quantity == 0 or level removed).
    //       Verify best_bid() reverts to sentinel / next-best level.
}

TEST(OrderBookTest, MultipleLevels) {
    // TODO: Add bids at prices 99.00, 100.00, 100.50.
    //       Add asks at prices 101.00, 102.00, 103.00.
    //       Verify best_bid() == 100.50 (highest bid).
    //       Verify best_ask() == 101.00 (lowest ask).
}

// ---------------------------------------------------------------------------
// TODO: Additional tests to add
// ---------------------------------------------------------------------------
// TEST(OrderBookTest, ModifyOrder)
//   — Modify quantity of an existing order; verify level quantity updated.
//
// TEST(OrderBookTest, AddAndCancelMultipleAtSameLevel)
//   — Add several orders at the same price, cancel one, verify level qty.
//
// TEST(OrderBookTest, LevelRemovedWhenEmpty)
//   — Cancel the last order at a level; verify the level node is removed.
//
// TEST(OrderBookTest, LargeBookDepth)
//   — Stress test: add 10 000 levels, verify best_bid/best_ask correct.
