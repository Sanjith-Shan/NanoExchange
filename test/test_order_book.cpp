#include <gtest/gtest.h>

#include <vector>

#include "nano/order_book.hpp"
#include "nano/types.hpp"

using namespace nano;

namespace {

// A fixture that pairs a pool with a book so tests read cleanly.
class BookTest : public ::testing::Test {
protected:
    OrderPool pool;
    OrderBook book{&pool, 0};

    std::vector<Trade> add(OrderId id, Side side, OrderType type, Price price, Quantity qty) {
        Order* o = pool.allocate(id, side, type, price, qty, now(), SymbolId{0});
        EXPECT_NE(o, nullptr);
        return book.add_order(o);
    }
};

} // namespace

TEST_F(BookTest, AddLimitBuyRestsInBids) {
    auto trades = add(1, Side::Buy, OrderType::Limit, 100, 10);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), INVALID_PRICE);
    EXPECT_EQ(book.bid_levels(), 1u);
    EXPECT_TRUE(book.contains(1));
}

TEST_F(BookTest, AddLimitSellRestsInAsks) {
    auto trades = add(1, Side::Sell, OrderType::Limit, 100, 10);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.best_ask(), 100);
    EXPECT_EQ(book.best_bid(), INVALID_PRICE);
    EXPECT_EQ(book.ask_levels(), 1u);
}

TEST_F(BookTest, MatchCross) {
    add(1, Side::Buy, OrderType::Limit, 100, 10);
    auto trades = add(2, Side::Sell, OrderType::Limit, 100, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(trades[0].maker_id, 1u);
    EXPECT_EQ(trades[0].taker_id, 2u);
    EXPECT_EQ(trades[0].taker_side, Side::Sell);
    // Both orders fully filled so the book is empty.
    EXPECT_EQ(book.total_orders(), 0u);
    EXPECT_EQ(book.best_bid(), INVALID_PRICE);
    EXPECT_EQ(book.best_ask(), INVALID_PRICE);
}

TEST_F(BookTest, MatchPriceImprovement) {
    // Resting sell at 100. Incoming buy willing to pay 105 trades at maker 100.
    add(1, Side::Sell, OrderType::Limit, 100, 10);
    auto trades = add(2, Side::Buy, OrderType::Limit, 105, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].quantity, 10u);
}

TEST_F(BookTest, MatchPartialFill) {
    add(1, Side::Sell, OrderType::Limit, 100, 10);
    // Incoming buy for 4 partially takes the resting sell.
    auto trades = add(2, Side::Buy, OrderType::Limit, 100, 4);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 4u);
    // Six remain resting on the ask side.
    EXPECT_EQ(book.best_ask(), 100);
    auto snap = book.snapshot(5);
    ASSERT_EQ(snap.asks.size(), 1u);
    EXPECT_EQ(snap.asks[0].quantity, 6u);
}

TEST_F(BookTest, MatchMultipleLevels) {
    // Three ask levels. An aggressive buy sweeps through all of them.
    add(1, Side::Sell, OrderType::Limit, 100, 5);
    add(2, Side::Sell, OrderType::Limit, 101, 5);
    add(3, Side::Sell, OrderType::Limit, 102, 5);

    auto trades = add(4, Side::Buy, OrderType::Limit, 102, 15);
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[1].price, 101);
    EXPECT_EQ(trades[2].price, 102);
    EXPECT_EQ(book.ask_levels(), 0u);
    EXPECT_EQ(book.total_orders(), 0u);
}

TEST_F(BookTest, PriceTimePriority) {
    // Two sells at the same price. The older one must fill first.
    add(1, Side::Sell, OrderType::Limit, 100, 5);
    add(2, Side::Sell, OrderType::Limit, 100, 5);
    auto trades = add(3, Side::Buy, OrderType::Limit, 100, 10);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_id, 1u); // Oldest resting order trades first.
    EXPECT_EQ(trades[1].maker_id, 2u);
}

TEST_F(BookTest, CancelOrderRemovesFromBook) {
    add(1, Side::Buy, OrderType::Limit, 100, 10);
    EXPECT_TRUE(book.cancel_order(1));
    EXPECT_FALSE(book.contains(1));
    EXPECT_EQ(book.best_bid(), INVALID_PRICE);
    EXPECT_EQ(book.bid_levels(), 0u);
}

TEST_F(BookTest, CancelNonexistentReturnsFalse) {
    EXPECT_FALSE(book.cancel_order(999));
}

TEST_F(BookTest, ModifyOrderChangesPlacementAndLosesTimePriority) {
    // Two bids at 100. Modifying the older one re prices it and sends it to the
    // back of its new level, so a later match hits the untouched order first.
    add(1, Side::Buy, OrderType::Limit, 100, 5);
    add(2, Side::Buy, OrderType::Limit, 100, 5);

    // Move order 1 up to 101 then back to 100 with fresh time priority.
    book.modify_order(1, 5, 100);

    // A sell of 5 should hit order 2 first because order 1 lost its priority.
    auto trades = add(3, Side::Sell, OrderType::Limit, 100, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 2u);
}

TEST_F(BookTest, ModifyOrderRepricesLevel) {
    add(1, Side::Buy, OrderType::Limit, 100, 10);
    book.modify_order(1, 10, 95);
    EXPECT_EQ(book.best_bid(), 95);
    EXPECT_TRUE(book.contains(1));
}

TEST_F(BookTest, IocPartial) {
    add(1, Side::Sell, OrderType::Limit, 100, 5);
    // IOC buy for 10 fills 5 and cancels the remainder. Nothing rests.
    auto trades = add(2, Side::Buy, OrderType::ImmediateOrCancel, 100, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 5u);
    EXPECT_EQ(book.best_bid(), INVALID_PRICE);
    EXPECT_FALSE(book.contains(2));
    EXPECT_EQ(book.total_orders(), 0u);
}

TEST_F(BookTest, IocNoMatch) {
    add(1, Side::Sell, OrderType::Limit, 100, 5);
    // IOC buy priced below the ask cannot trade and must not rest.
    auto trades = add(2, Side::Buy, OrderType::ImmediateOrCancel, 99, 5);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.best_bid(), INVALID_PRICE);
    EXPECT_FALSE(book.contains(2));
    // The resting sell is untouched.
    EXPECT_EQ(book.best_ask(), 100);
}

TEST_F(BookTest, FokFullFill) {
    add(1, Side::Sell, OrderType::Limit, 100, 5);
    add(2, Side::Sell, OrderType::Limit, 101, 5);
    // FOK buy for 10 can be fully satisfied so it executes.
    auto trades = add(3, Side::Buy, OrderType::FillOrKill, 101, 10);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].quantity, 5u);
    EXPECT_EQ(trades[1].quantity, 5u);
    EXPECT_EQ(book.ask_levels(), 0u);
}

TEST_F(BookTest, FokInsufficient) {
    add(1, Side::Sell, OrderType::Limit, 100, 5);
    // FOK buy for 10 cannot be fully filled so it is rejected entirely.
    auto trades = add(2, Side::Buy, OrderType::FillOrKill, 100, 10);
    EXPECT_TRUE(trades.empty());
    // The book is unchanged.
    EXPECT_EQ(book.best_ask(), 100);
    auto snap = book.snapshot(5);
    ASSERT_EQ(snap.asks.size(), 1u);
    EXPECT_EQ(snap.asks[0].quantity, 5u);
    EXPECT_FALSE(book.contains(2));
}

TEST_F(BookTest, MarketBuySweepsAsks) {
    add(1, Side::Sell, OrderType::Limit, 100, 5);
    add(2, Side::Sell, OrderType::Limit, 101, 5);
    auto trades = add(3, Side::Buy, OrderType::Market, INVALID_PRICE, 8);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].quantity, 5u);
    EXPECT_EQ(trades[1].price, 101);
    EXPECT_EQ(trades[1].quantity, 3u);
    // Remainder of the market order never rests. Two remain at 101.
    EXPECT_FALSE(book.contains(3));
    auto snap = book.snapshot(5);
    ASSERT_EQ(snap.asks.size(), 1u);
    EXPECT_EQ(snap.asks[0].price, 101);
    EXPECT_EQ(snap.asks[0].quantity, 2u);
}

TEST_F(BookTest, MarketSellSweepsBids) {
    add(1, Side::Buy, OrderType::Limit, 100, 5);
    add(2, Side::Buy, OrderType::Limit, 99, 5);
    auto trades = add(3, Side::Sell, OrderType::Market, INVALID_PRICE, 7);
    ASSERT_EQ(trades.size(), 2u);
    // Best bid is highest so 100 trades before 99.
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[1].price, 99);
    EXPECT_EQ(trades[1].quantity, 2u);
}

TEST_F(BookTest, EmptyBookMarket) {
    // A market order against an empty book produces no trades and does not crash.
    auto trades = add(1, Side::Buy, OrderType::Market, INVALID_PRICE, 10);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.total_orders(), 0u);
}

TEST_F(BookTest, BestBidAskCorrectness) {
    add(1, Side::Buy, OrderType::Limit, 98, 5);
    add(2, Side::Buy, OrderType::Limit, 100, 5);
    add(3, Side::Buy, OrderType::Limit, 99, 5);
    add(4, Side::Sell, OrderType::Limit, 105, 5);
    add(5, Side::Sell, OrderType::Limit, 103, 5);
    add(6, Side::Sell, OrderType::Limit, 104, 5);

    EXPECT_EQ(book.best_bid(), 100); // Highest bid.
    EXPECT_EQ(book.best_ask(), 103); // Lowest ask.
}

TEST_F(BookTest, SpreadCorrectness) {
    add(1, Side::Buy, OrderType::Limit, 100, 5);
    add(2, Side::Sell, OrderType::Limit, 103, 5);
    EXPECT_EQ(book.spread(), 3);
}

TEST_F(BookTest, SpreadZeroWhenOneSideEmpty) {
    add(1, Side::Buy, OrderType::Limit, 100, 5);
    EXPECT_EQ(book.spread(), 0);
}

TEST_F(BookTest, SnapshotDepthAccuracy) {
    // Two orders at 100 on the bid, one at 99. Aggregation and ordering checks.
    add(1, Side::Buy, OrderType::Limit, 100, 5);
    add(2, Side::Buy, OrderType::Limit, 100, 7);
    add(3, Side::Buy, OrderType::Limit, 99, 4);
    add(4, Side::Sell, OrderType::Limit, 101, 3);
    add(5, Side::Sell, OrderType::Limit, 102, 6);

    auto snap = book.snapshot(5);
    ASSERT_EQ(snap.bids.size(), 2u);
    // Best bid first.
    EXPECT_EQ(snap.bids[0].price, 100);
    EXPECT_EQ(snap.bids[0].quantity, 12u); // 5 + 7 aggregated.
    EXPECT_EQ(snap.bids[0].order_count, 2u);
    EXPECT_EQ(snap.bids[1].price, 99);
    EXPECT_EQ(snap.bids[1].quantity, 4u);
    EXPECT_EQ(snap.bids[1].order_count, 1u);

    ASSERT_EQ(snap.asks.size(), 2u);
    // Best ask first.
    EXPECT_EQ(snap.asks[0].price, 101);
    EXPECT_EQ(snap.asks[0].quantity, 3u);
    EXPECT_EQ(snap.asks[1].price, 102);
    EXPECT_EQ(snap.asks[1].quantity, 6u);

    EXPECT_EQ(snap.best_bid, 100);
    EXPECT_EQ(snap.best_ask, 101);
    EXPECT_EQ(snap.spread, 1);
}

TEST_F(BookTest, SnapshotRespectsDepthLimit) {
    for (Price p = 100; p >= 95; --p) {
        add(static_cast<OrderId>(p), Side::Buy, OrderType::Limit, p, 1);
    }
    auto snap = book.snapshot(3);
    // Only the top three levels are reported.
    ASSERT_EQ(snap.bids.size(), 3u);
    EXPECT_EQ(snap.bids[0].price, 100);
    EXPECT_EQ(snap.bids[1].price, 99);
    EXPECT_EQ(snap.bids[2].price, 98);
}
