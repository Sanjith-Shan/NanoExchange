// End to end matching throughput. A book is preloaded with a depth of resting
// sell orders and then M aggressive crossing buys are fired through it. Each buy
// carries exactly one lot so it fully fills exactly one resting order, making the
// work per aggressor constant. The M consumed levels are refilled in a paused
// region so the book depth is stable across iterations. Throughput is reported
// through SetItemsProcessed.

#include <benchmark/benchmark.h>

#include "nano/order_book.hpp"
#include "nano/types.hpp"

#include <algorithm>
#include <memory>
#include <vector>

using namespace nano;

namespace {

constexpr SymbolId kSym        = 7;
constexpr Price    kAskBase    = 1'000'000;
constexpr Quantity kQty        = 100;
constexpr int64_t  kAggressors = 10'000; // M crossing orders per iteration.

} // namespace

// N is the resting depth. We rest at least M orders so a single batch of M buys
// never runs the book dry. Each iteration consumes the M lowest levels and then
// re posts them in the paused region.
static void BM_MatchThroughput(benchmark::State& state) {
    const int64_t n       = state.range(0);
    const int64_t resting = std::max<int64_t>(n, kAggressors);

    auto pool = std::make_unique<OrderPool>();
    OrderBook book{pool.get(), kSym};
    OrderId next_id = 1;

    for (int64_t i = 0; i < resting; ++i) {
        Order* o = pool->allocate(next_id++, Side::Sell, OrderType::Limit,
                                  kAskBase + i, kQty, now(), kSym);
        book.add_order(o);
    }

    // A price above the whole resting window so every buy crosses. The single lot
    // per buy is what bounds each aggressor to one fill.
    const Price buy_price = kAskBase + resting + 1;

    std::vector<Order*> takers;
    takers.reserve(static_cast<std::size_t>(kAggressors));

    for (auto _ : state) {
        state.PauseTiming();
        takers.clear();
        for (int64_t i = 0; i < kAggressors; ++i) {
            takers.push_back(pool->allocate(next_id++, Side::Buy, OrderType::Limit,
                                            buy_price, kQty, now(), kSym));
        }
        state.ResumeTiming();

        for (Order* t : takers) {
            auto trades = book.add_order(t);
            benchmark::DoNotOptimize(trades.data());
        }

        state.PauseTiming();
        // Re post the M lowest levels that were just consumed.
        for (int64_t i = 0; i < kAggressors; ++i) {
            Order* o = pool->allocate(next_id++, Side::Sell, OrderType::Limit,
                                      kAskBase + i, kQty, now(), kSym);
            book.add_order(o);
        }
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * kAggressors);
}
BENCHMARK(BM_MatchThroughput)->Arg(100)->Arg(1'000)->Arg(10'000)->Arg(100'000);
