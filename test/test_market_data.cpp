#include <gtest/gtest.h>

#include "nano/market_data.hpp"
#include "nano/order_book.hpp"
#include "nano/types.hpp"

using namespace nano;

namespace {

class MarketDataTest : public ::testing::Test {
protected:
    OrderPool pool;
    OrderBook book{&pool, 0};

    void add(OrderId id, Side side, Price price, Quantity qty) {
        Order* o = pool.allocate(id, side, OrderType::Limit, price, qty, now(), SymbolId{0});
        ASSERT_NE(o, nullptr);
        book.add_order(o);
    }
};

} // namespace

TEST_F(MarketDataTest, SnapshotOnEmptyBook) {
    auto snap = book.snapshot(5);
    EXPECT_EQ(snap.best_bid, INVALID_PRICE);
    EXPECT_EQ(snap.best_ask, INVALID_PRICE);
    EXPECT_EQ(snap.spread, 0);
    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());
}

TEST_F(MarketDataTest, SnapshotSeveralLevelsOrderingAndAggregation) {
    // Bids at 100 (two orders), 99, 98. Asks at 101, 102, 103.
    add(1, Side::Buy, 100, 5);
    add(2, Side::Buy, 100, 5);
    add(3, Side::Buy, 99, 3);
    add(4, Side::Buy, 98, 2);
    add(5, Side::Sell, 101, 4);
    add(6, Side::Sell, 102, 6);
    add(7, Side::Sell, 103, 8);

    auto snap = book.snapshot(5);

    ASSERT_EQ(snap.bids.size(), 3u);
    EXPECT_EQ(snap.bids[0].price, 100);
    EXPECT_EQ(snap.bids[0].quantity, 10u);   // Aggregated across both orders.
    EXPECT_EQ(snap.bids[0].order_count, 2u);
    EXPECT_EQ(snap.bids[1].price, 99);
    EXPECT_EQ(snap.bids[2].price, 98);
    // Best first means strictly descending bid prices.
    EXPECT_GT(snap.bids[0].price, snap.bids[1].price);
    EXPECT_GT(snap.bids[1].price, snap.bids[2].price);

    ASSERT_EQ(snap.asks.size(), 3u);
    EXPECT_EQ(snap.asks[0].price, 101);
    EXPECT_EQ(snap.asks[1].price, 102);
    EXPECT_EQ(snap.asks[2].price, 103);
    // Best first means strictly ascending ask prices.
    EXPECT_LT(snap.asks[0].price, snap.asks[1].price);
    EXPECT_LT(snap.asks[1].price, snap.asks[2].price);

    EXPECT_EQ(snap.best_bid, 100);
    EXPECT_EQ(snap.best_ask, 101);
    EXPECT_EQ(snap.spread, 1);
}

TEST_F(MarketDataTest, SnapshotDepthLimit) {
    for (Price p = 110; p <= 115; ++p) add(static_cast<OrderId>(p), Side::Sell, p, 1);
    auto snap = book.snapshot(2);
    ASSERT_EQ(snap.asks.size(), 2u);
    EXPECT_EQ(snap.asks[0].price, 110);
    EXPECT_EQ(snap.asks[1].price, 111);
}
