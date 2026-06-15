// Microbenchmarks for individual OrderBook operations, parameterised by resting
// book density N. State that an operation mutates is rebuilt in paused regions
// so only the intended op is timed. All setup that can run once per invocation
// runs before the timing loop.

#include <benchmark/benchmark.h>

#include "nano/order_book.hpp"
#include "nano/types.hpp"

#include <memory>
#include <random>
#include <vector>

using namespace nano;

namespace {

constexpr SymbolId kSym     = 1;
constexpr Price    kBidBase = 2'000'000; // Bids sit below this, walking downward.
constexpr Price    kAskBase = 3'000'000; // Asks sit above this, walking upward.
constexpr Quantity kQty     = 100;

// One order per distinct price level. Bids only. Ids and prices are returned so
// callers can churn the book without losing track of what is resting.
struct BidBook {
    std::unique_ptr<OrderPool> pool = std::make_unique<OrderPool>();
    OrderBook book{pool.get(), kSym};
    std::vector<OrderId> ids;
    std::vector<Price>   prices;
    OrderId next_id = 1;
};

BidBook build_bids(int64_t n) {
    BidBook b;
    b.ids.reserve(static_cast<std::size_t>(n));
    b.prices.reserve(static_cast<std::size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        const Price p = kBidBase - i;
        Order* o = b.pool->allocate(b.next_id, Side::Buy, OrderType::Limit, p, kQty, now(), kSym);
        b.book.add_order(o);
        b.ids.push_back(b.next_id);
        b.prices.push_back(p);
        ++b.next_id;
    }
    return b;
}

// Both sides with a wide gap so nothing crosses. Used by read only benchmarks.
BidBook build_both(int64_t n) {
    BidBook b;
    for (int64_t i = 0; i < n; ++i) {
        const Price bp = kBidBase - i;
        const Price ap = kAskBase + i;
        Order* bid = b.pool->allocate(b.next_id++, Side::Buy, OrderType::Limit, bp, kQty, now(), kSym);
        b.book.add_order(bid);
        Order* ask = b.pool->allocate(b.next_id++, Side::Sell, OrderType::Limit, ap, kQty, now(), kSym);
        b.book.add_order(ask);
    }
    return b;
}

void args(benchmark::internal::Benchmark* bm) {
    bm->Arg(100)->Arg(1'000)->Arg(10'000)->Arg(100'000);
}

} // namespace

// Add a non crossing limit into a book of N resting bids. The previous add is
// cancelled and the next order allocated inside a paused region so the book
// stays at N and only add_order is timed.
static void BM_AddLimit(benchmark::State& state) {
    BidBook b = build_bids(state.range(0));
    const Price add_price = kBidBase - state.range(0) - 10;
    bool have_prev = false;
    OrderId prev_id = 0;

    for (auto _ : state) {
        state.PauseTiming();
        if (have_prev) b.book.cancel_order(prev_id);
        const OrderId id = b.next_id++;
        Order* o = b.pool->allocate(id, Side::Buy, OrderType::Limit, add_price, kQty, now(), kSym);
        state.ResumeTiming();

        auto trades = b.book.add_order(o);
        benchmark::DoNotOptimize(trades.data());

        prev_id = id;
        have_prev = true;
    }
}
BENCHMARK(BM_AddLimit)->Apply(args);

// Add an aggressive limit that immediately crosses and fully fills one resting
// order. A fresh best ask is posted in the paused region each iteration so the
// timed add always finds a maker to hit while the N deep background is untouched.
static void BM_AddAndMatch(benchmark::State& state) {
    const int64_t n = state.range(0);
    BidBook b; // Background asks only.
    for (int64_t i = 1; i <= n; ++i) {
        Order* o = b.pool->allocate(b.next_id++, Side::Sell, OrderType::Limit,
                                    kAskBase + i, kQty, now(), kSym);
        b.book.add_order(o);
    }

    for (auto _ : state) {
        state.PauseTiming();
        Order* maker = b.pool->allocate(b.next_id++, Side::Sell, OrderType::Limit,
                                        kAskBase, kQty, now(), kSym);
        b.book.add_order(maker); // Best ask.
        Order* taker = b.pool->allocate(b.next_id++, Side::Buy, OrderType::Limit,
                                        kAskBase, kQty, now(), kSym);
        state.ResumeTiming();

        auto trades = b.book.add_order(taker); // Crosses the maker, full fill.
        benchmark::DoNotOptimize(trades.data());
    }
}
BENCHMARK(BM_AddAndMatch)->Apply(args);

// Cancel a random resting order. The cancelled slot is refilled in the paused
// region so the book stays at N.
static void BM_Cancel(benchmark::State& state) {
    BidBook b = build_bids(state.range(0));
    std::mt19937_64 rng(0xC0FFEE);

    for (auto _ : state) {
        state.PauseTiming();
        const std::size_t idx = rng() % b.ids.size();
        const OrderId victim = b.ids[idx];
        state.ResumeTiming();

        bool ok = b.book.cancel_order(victim);
        benchmark::DoNotOptimize(ok);

        state.PauseTiming();
        const OrderId nid = b.next_id++;
        Order* o = b.pool->allocate(nid, Side::Buy, OrderType::Limit, b.prices[idx], kQty, now(), kSym);
        b.book.add_order(o);
        b.ids[idx] = nid;
        state.ResumeTiming();
    }
}
BENCHMARK(BM_Cancel)->Apply(args);

// Modify a random resting order in place, alternating quantity. modify_order
// cancels and re adds internally so the book size is preserved with no restore.
static void BM_Modify(benchmark::State& state) {
    BidBook b = build_bids(state.range(0));
    std::mt19937_64 rng(0xBEEF);
    Quantity q = kQty;

    for (auto _ : state) {
        state.PauseTiming();
        const std::size_t idx = rng() % b.ids.size();
        const OrderId id = b.ids[idx];
        const Price p = b.prices[idx];
        q = (q == kQty) ? kQty * 2 : kQty;
        state.ResumeTiming();

        auto trades = b.book.modify_order(id, q, p);
        benchmark::DoNotOptimize(trades.data());
    }
}
BENCHMARK(BM_Modify)->Apply(args);

// Read best bid and best ask off a two sided book of depth N.
static void BM_BestBidAsk(benchmark::State& state) {
    BidBook b = build_both(state.range(0));
    for (auto _ : state) {
        Price bid = b.book.best_bid();
        Price ask = b.book.best_ask();
        benchmark::DoNotOptimize(bid);
        benchmark::DoNotOptimize(ask);
    }
}
BENCHMARK(BM_BestBidAsk)->Apply(args);

// Build a five level depth snapshot of a two sided book of depth N.
static void BM_Snapshot(benchmark::State& state) {
    BidBook b = build_both(state.range(0));
    for (auto _ : state) {
        auto snap = b.book.snapshot(5);
        benchmark::DoNotOptimize(snap.bids.data());
        benchmark::DoNotOptimize(snap.asks.data());
    }
}
BENCHMARK(BM_Snapshot)->Apply(args);
