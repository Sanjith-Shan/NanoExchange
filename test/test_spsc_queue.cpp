#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "nano/spsc_queue.hpp"

using namespace nano;

TEST(SPSCQueue, PushPopSingleThread) {
    SPSCQueue<int, 8> q;
    EXPECT_TRUE(q.empty());
    EXPECT_TRUE(q.try_push(42));
    EXPECT_FALSE(q.empty());

    int out = 0;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 42);
    EXPECT_TRUE(q.empty());
}

TEST(SPSCQueue, EmptyPopReturnsFalse) {
    SPSCQueue<int, 8> q;
    int out = -1;
    EXPECT_FALSE(q.try_pop(out));
}

TEST(SPSCQueue, FullPushReturnsFalse) {
    // Capacity is rounded to 8 but only 7 usable slots exist.
    SPSCQueue<int, 8> q;
    EXPECT_EQ(q.capacity(), 7u);
    for (int i = 0; i < 7; ++i) {
        EXPECT_TRUE(q.try_push(i));
    }
    // The eighth push must fail because the ring is full.
    EXPECT_FALSE(q.try_push(999));
}

TEST(SPSCQueue, WrapAround) {
    SPSCQueue<int, 8> q;
    // Push and pop enough to wrap the ring several times.
    int expected = 0;
    for (int round = 0; round < 100; ++round) {
        EXPECT_TRUE(q.try_push(round));
        int out = -1;
        EXPECT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, expected);
        ++expected;
    }
    EXPECT_TRUE(q.empty());
}

TEST(SPSCQueue, ConcurrentProducerConsumer) {
    constexpr int N = 100000;
    SPSCQueue<int, 1024> q;
    std::vector<int> received;
    received.reserve(N);

    std::thread producer([&] {
        for (int i = 0; i < N; ++i) {
            while (!q.try_push(i)) {
                // Retry on full.
            }
        }
    });

    std::thread consumer([&] {
        int got = 0;
        int out = 0;
        while (got < N) {
            if (q.try_pop(out)) {
                received.push_back(out);
                ++got;
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(N));
    // The queue preserves order so we must see 0..N-1 in sequence.
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(received[static_cast<size_t>(i)], i);
    }
}
