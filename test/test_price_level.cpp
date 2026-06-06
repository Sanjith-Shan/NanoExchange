#include <gtest/gtest.h>

#include "nano/order.hpp"
#include "nano/price_level.hpp"

using namespace nano;

// PriceLevel only threads pointers so stack orders are fine here.
namespace {

Order make_order(OrderId id, Quantity qty) {
    Order o;
    o.id = id;
    o.remaining_qty = qty;
    o.initial_qty = qty;
    o.active = true;
    return o;
}

} // namespace

TEST(PriceLevel, AppendAndFrontFifoOrder) {
    PriceLevel level(100);
    Order a = make_order(1, 10);
    Order b = make_order(2, 20);
    Order c = make_order(3, 30);

    level.append(&a);
    level.append(&b);
    level.append(&c);

    // Oldest order sits at the front.
    EXPECT_EQ(level.front(), &a);
    EXPECT_EQ(level.order_count(), 3u);
    EXPECT_EQ(level.total_quantity(), 60u);
}

TEST(PriceLevel, RemoveFrontAdvancesFifo) {
    PriceLevel level(100);
    Order a = make_order(1, 10);
    Order b = make_order(2, 20);
    level.append(&a);
    level.append(&b);

    level.remove(&a);
    EXPECT_EQ(level.front(), &b);
    EXPECT_EQ(level.order_count(), 1u);
    EXPECT_EQ(level.total_quantity(), 20u);
}

TEST(PriceLevel, RemoveMiddleKeepsIntegrity) {
    PriceLevel level(100);
    Order a = make_order(1, 10);
    Order b = make_order(2, 20);
    Order c = make_order(3, 30);
    level.append(&a);
    level.append(&b);
    level.append(&c);

    level.remove(&b);
    EXPECT_EQ(level.order_count(), 2u);
    EXPECT_EQ(level.total_quantity(), 40u);
    // Front still oldest, and the list stitches a to c.
    EXPECT_EQ(level.front(), &a);
    EXPECT_EQ(a.next, &c);
    EXPECT_EQ(c.prev, &a);
}

TEST(PriceLevel, TotalQuantityAcrossAppendsAndRemoves) {
    PriceLevel level(100);
    Order a = make_order(1, 5);
    Order b = make_order(2, 15);
    Order c = make_order(3, 25);
    level.append(&a);
    EXPECT_EQ(level.total_quantity(), 5u);
    level.append(&b);
    EXPECT_EQ(level.total_quantity(), 20u);
    level.append(&c);
    EXPECT_EQ(level.total_quantity(), 45u);
    level.remove(&b);
    EXPECT_EQ(level.total_quantity(), 30u);
}

TEST(PriceLevel, EmptyAfterRemovingAll) {
    PriceLevel level(100);
    Order a = make_order(1, 10);
    Order b = make_order(2, 20);
    level.append(&a);
    level.append(&b);

    EXPECT_FALSE(level.empty());
    level.remove(&a);
    level.remove(&b);

    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.order_count(), 0u);
    EXPECT_EQ(level.total_quantity(), 0u);
    EXPECT_EQ(level.front(), nullptr);
}

TEST(PriceLevel, PriceAccessor) {
    PriceLevel level(12345);
    EXPECT_EQ(level.price(), 12345);
}
