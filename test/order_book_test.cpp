// Unit tests for engine::OrderBook
//
// Covers: add/cancel, duplicate and out-of-range rejection, best bid/ask
// tracking (including rescans after cancelling the best level), FIFO order
// within a level, and level aggregate bookkeeping. These tests also exercise
// PriceLevel's intrusive list end to end

#include <gtest/gtest.h>
#include "core/order_book.h"
#include "core/types.h"

#include <deque>

namespace {

using namespace engine;

class OrderBookTest : public ::testing::Test {
protected:
    // deque gives stable addresses on push_back, so Order* pointers held by
    // the book stay valid as more test orders are created
    Order* make_order(OrderId id, Side side, Price price, Quantity qty) {
        Order& o = orders_.emplace_back();
        o.id       = id;
        o.side     = side;
        o.price    = price;
        o.quantity = qty;
        return &o;
    }

    OrderBook          book_;
    std::deque<Order>  orders_;
};

TEST_F(OrderBookTest, EmptyBook) {
    EXPECT_FALSE(book_.has_bids());
    EXPECT_FALSE(book_.has_asks());
    EXPECT_EQ(book_.bid_count(), 0u);
    EXPECT_EQ(book_.ask_count(), 0u);
    EXPECT_EQ(book_.best_bid(), 0);
    EXPECT_EQ(book_.best_ask(), MAX_PRICE_TICKS - 1);
}

TEST_F(OrderBookTest, AddSingleBidOrder) {
    ASSERT_TRUE(book_.add_order(make_order(1, Side::Bid, 10'000, 50)));

    EXPECT_TRUE(book_.has_bids());
    EXPECT_FALSE(book_.has_asks());
    EXPECT_EQ(book_.best_bid(), 10'000);
    EXPECT_EQ(book_.get_level(10'000).total_quantity(), 50u);
    EXPECT_EQ(book_.get_level(10'000).order_count(), 1u);
}

TEST_F(OrderBookTest, AddSingleAskOrder) {
    ASSERT_TRUE(book_.add_order(make_order(1, Side::Ask, 10'150, 25)));

    EXPECT_TRUE(book_.has_asks());
    EXPECT_FALSE(book_.has_bids());
    EXPECT_EQ(book_.best_ask(), 10'150);
    EXPECT_EQ(book_.get_level(10'150).total_quantity(), 25u);
}

TEST_F(OrderBookTest, RejectsDuplicateId) {
    ASSERT_TRUE(book_.add_order(make_order(7, Side::Bid, 10'000, 10)));
    EXPECT_FALSE(book_.add_order(make_order(7, Side::Bid, 10'100, 20)));

    // the failed add must not have touched the book
    EXPECT_EQ(book_.bid_count(), 1u);
    EXPECT_EQ(book_.best_bid(), 10'000);
    EXPECT_TRUE(book_.get_level(10'100).empty());
}

TEST_F(OrderBookTest, RejectsOutOfRangePrice) {
    EXPECT_FALSE(book_.add_order(make_order(1, Side::Bid, -1, 10)));
    EXPECT_FALSE(book_.add_order(make_order(2, Side::Ask, MAX_PRICE_TICKS, 10)));
    EXPECT_FALSE(book_.has_bids());
    EXPECT_FALSE(book_.has_asks());
}

TEST_F(OrderBookTest, BestPriceTracksAcrossLevels) {
    book_.add_order(make_order(1, Side::Bid, 9'900,  10));
    book_.add_order(make_order(2, Side::Bid, 10'000, 10));
    book_.add_order(make_order(3, Side::Bid, 10'050, 10));
    book_.add_order(make_order(4, Side::Ask, 10'100, 10));
    book_.add_order(make_order(5, Side::Ask, 10'200, 10));

    EXPECT_EQ(book_.best_bid(), 10'050);  // highest bid
    EXPECT_EQ(book_.best_ask(), 10'100);  // lowest ask
    EXPECT_EQ(book_.spread(), 50);

    // a worse price must not move the best
    book_.add_order(make_order(6, Side::Bid, 9'800, 10));
    EXPECT_EQ(book_.best_bid(), 10'050);
}

TEST_F(OrderBookTest, GetOrderLookup) {
    Order* o = make_order(42, Side::Ask, 10'100, 5);
    book_.add_order(o);

    EXPECT_EQ(book_.get_order(42), o);
    EXPECT_EQ(book_.get_order(43), nullptr);
}

TEST_F(OrderBookTest, CancelOrder) {
    book_.add_order(make_order(1, Side::Bid, 10'000, 50));
    ASSERT_TRUE(book_.cancel_order(1));

    EXPECT_FALSE(book_.has_bids());
    EXPECT_EQ(book_.best_bid(), 0);  // sentinel restored
    EXPECT_TRUE(book_.get_level(10'000).empty());
    EXPECT_EQ(book_.get_order(1), nullptr);
}

TEST_F(OrderBookTest, CancelNonExistentOrder) {
    EXPECT_FALSE(book_.cancel_order(99));

    book_.add_order(make_order(1, Side::Bid, 10'000, 50));
    ASSERT_TRUE(book_.cancel_order(1));
    EXPECT_FALSE(book_.cancel_order(1));  // already gone
}

TEST_F(OrderBookTest, CancelBestRevealsNextLevel) {
    book_.add_order(make_order(1, Side::Bid, 9'900,  10));
    book_.add_order(make_order(2, Side::Bid, 10'000, 10));
    book_.add_order(make_order(3, Side::Ask, 10'100, 10));
    book_.add_order(make_order(4, Side::Ask, 10'200, 10));

    ASSERT_TRUE(book_.cancel_order(2));
    EXPECT_EQ(book_.best_bid(), 9'900);

    ASSERT_TRUE(book_.cancel_order(3));
    EXPECT_EQ(book_.best_ask(), 10'200);
}

TEST_F(OrderBookTest, CancelNonBestLeavesBestUntouched) {
    book_.add_order(make_order(1, Side::Bid, 9'900,  10));
    book_.add_order(make_order(2, Side::Bid, 10'000, 10));

    ASSERT_TRUE(book_.cancel_order(1));
    EXPECT_EQ(book_.best_bid(), 10'000);
}

TEST_F(OrderBookTest, CancelLastOrderOnSideResetsSentinel) {
    // asks remain in the book; cancelling the only bid must reset the bid
    // sentinel via the count short-circuit, not scan into ask territory
    book_.add_order(make_order(1, Side::Bid, 10'000, 10));
    book_.add_order(make_order(2, Side::Ask, 10'100, 10));

    ASSERT_TRUE(book_.cancel_order(1));
    EXPECT_FALSE(book_.has_bids());
    EXPECT_TRUE(book_.has_asks());
    EXPECT_EQ(book_.best_bid(), 0);
    EXPECT_EQ(book_.best_ask(), 10'100);
}

TEST_F(OrderBookTest, FifoOrderWithinLevel) {
    Order* first  = make_order(1, Side::Bid, 10'000, 10);
    Order* second = make_order(2, Side::Bid, 10'000, 20);
    Order* third  = make_order(3, Side::Bid, 10'000, 30);
    book_.add_order(first);
    book_.add_order(second);
    book_.add_order(third);

    const PriceLevel& level = book_.get_level(10'000);
    EXPECT_EQ(level.order_count(), 3u);
    EXPECT_EQ(level.total_quantity(), 60u);
    EXPECT_EQ(level.front(), first);  // oldest has time priority
}

TEST_F(OrderBookTest, CancelMiddleOrderPreservesFifo) {
    Order* first  = make_order(1, Side::Bid, 10'000, 10);
    Order* second = make_order(2, Side::Bid, 10'000, 20);
    Order* third  = make_order(3, Side::Bid, 10'000, 30);
    book_.add_order(first);
    book_.add_order(second);
    book_.add_order(third);

    ASSERT_TRUE(book_.cancel_order(2));

    const PriceLevel& level = book_.get_level(10'000);
    EXPECT_EQ(level.order_count(), 2u);
    EXPECT_EQ(level.total_quantity(), 40u);
    EXPECT_EQ(level.front(), first);
    EXPECT_EQ(first->next, third);  // list rewired around the removed order
    EXPECT_EQ(third->prev, first);
    EXPECT_EQ(book_.best_bid(), 10'000);  // level not empty, best unchanged
}

TEST_F(OrderBookTest, DeepBookCancelWalk) {
    // 100 bid levels; cancel the best one repeatedly and verify the best
    // pointer walks down level by level
    for (OrderId i = 0; i < 100; ++i) {
        book_.add_order(make_order(i + 1, Side::Bid, 10'000 + static_cast<Price>(i), 10));
    }
    EXPECT_EQ(book_.best_bid(), 10'099);

    for (OrderId i = 100; i > 1; --i) {
        ASSERT_TRUE(book_.cancel_order(i));
        EXPECT_EQ(book_.best_bid(), 10'000 + static_cast<Price>(i) - 2);
    }

    ASSERT_TRUE(book_.cancel_order(1));
    EXPECT_FALSE(book_.has_bids());
}

} // namespace
