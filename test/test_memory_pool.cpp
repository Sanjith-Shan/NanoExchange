#include <gtest/gtest.h>

#include <vector>

#include "nano/memory_pool.hpp"
#include "nano/order.hpp"

using namespace nano;

namespace {

// allocate forwards its arguments to the Order constructor.
Order* alloc(MemoryPool<Order, 4>& pool, OrderId id) {
    return pool.allocate(id, Side::Buy, OrderType::Limit, Price{100}, Quantity{10}, Timestamp{0});
}

} // namespace

TEST(MemoryPool, StartsEmpty) {
    MemoryPool<Order, 4> pool;
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_EQ(pool.capacity(), 4u);
    EXPECT_TRUE(pool.empty());
    EXPECT_FALSE(pool.full());
}

TEST(MemoryPool, AllocateDeallocateCycle) {
    MemoryPool<Order, 4> pool;
    Order* o = alloc(pool, 1);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->id, 1u);
    EXPECT_EQ(pool.size(), 1u);
    EXPECT_FALSE(pool.empty());

    pool.deallocate(o);
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_TRUE(pool.empty());
}

TEST(MemoryPool, Exhaustion) {
    MemoryPool<Order, 4> pool;
    std::vector<Order*> held;
    for (int i = 0; i < 4; ++i) {
        Order* o = alloc(pool, static_cast<OrderId>(i + 1));
        ASSERT_NE(o, nullptr);
        held.push_back(o);
    }
    EXPECT_TRUE(pool.full());
    // The pool is exhausted so the next allocate must return nullptr.
    EXPECT_EQ(alloc(pool, 99), nullptr);

    for (Order* o : held) pool.deallocate(o);
}

TEST(MemoryPool, ReuseAfterDeallocate) {
    MemoryPool<Order, 4> pool;
    Order* first = alloc(pool, 1);
    void* addr = static_cast<void*>(first);
    pool.deallocate(first);

    Order* second = alloc(pool, 2);
    // The freed slot is at the head of the free list so it comes back first.
    EXPECT_EQ(static_cast<void*>(second), addr);
    EXPECT_EQ(second->id, 2u);
    pool.deallocate(second);
}

TEST(MemoryPool, AllocateAllDeallocateAllReallocateAll) {
    MemoryPool<Order, 4> pool;
    std::vector<Order*> held;
    for (int i = 0; i < 4; ++i) held.push_back(alloc(pool, static_cast<OrderId>(i + 1)));
    EXPECT_TRUE(pool.full());
    EXPECT_EQ(pool.size(), 4u);

    for (Order* o : held) pool.deallocate(o);
    EXPECT_TRUE(pool.empty());
    EXPECT_EQ(pool.size(), 0u);

    held.clear();
    for (int i = 0; i < 4; ++i) {
        Order* o = alloc(pool, static_cast<OrderId>(i + 10));
        ASSERT_NE(o, nullptr);
        held.push_back(o);
    }
    EXPECT_TRUE(pool.full());
    for (Order* o : held) pool.deallocate(o);
}

TEST(MemoryPool, DeallocateNullIsSafe) {
    MemoryPool<Order, 4> pool;
    pool.deallocate(nullptr);
    EXPECT_EQ(pool.size(), 0u);
}
