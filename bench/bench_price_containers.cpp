// Head to head of the three price level containers under three workloads. The
// active level count L is swept so the crossover between the cache friendly array
// and vector at small L and the tree at large L becomes visible. Prices are drawn
// from a fixed seed generator in [0, L) so every container sees identical input
// and the array window is bounded.
//
// Container construction differs. The array eagerly allocates a slot per price in
// the window while the map and vector start empty, so the array carries that
// build cost in the mutating workloads. That is a real property of the design and
// is left in rather than hidden.

#include <benchmark/benchmark.h>

#include "nano/price_containers.hpp"
#include "nano/price_level.hpp"
#include "nano/types.hpp"

#include <cstdint>
#include <random>
#include <vector>

using namespace nano;

namespace {

constexpr std::size_t kOps = 100'000; // Operations per iteration for each workload.
constexpr uint64_t    kSeed = 0x9E3779B97F4A7C15ull;

// Factories. Map and vector ignore the window, the array needs it.
auto make_map(int64_t)     { return MapContainer<true>{}; }
auto make_vec(int64_t)     { return SortedVectorContainer<true>{}; }
auto make_arr(int64_t L)   { return ArrayContainer<true>(0, static_cast<std::size_t>(L)); }

void args(benchmark::internal::Benchmark* bm) {
    bm->Arg(10)->Arg(100)->Arg(1'000)->Arg(10'000);
}

} // namespace

// (a) Insert heavy. A fresh container is filled with kOps random priced levels.
template <typename Make>
void insert_heavy(benchmark::State& state, Make make) {
    const int64_t L = state.range(0);
    std::mt19937_64 rng(kSeed);
    std::uniform_int_distribution<int64_t> dist(0, L - 1);
    std::vector<Price> ps(kOps);
    for (auto& p : ps) p = dist(rng);

    for (auto _ : state) {
        auto c = make(L);
        for (std::size_t i = 0; i < kOps; ++i) c.insert(ps[i], PriceLevel(ps[i]));
        benchmark::DoNotOptimize(&c);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kOps));
}

// (b) Mixed. Sixty percent insert, thirty percent erase, ten percent best.
template <typename Make>
void mixed(benchmark::State& state, Make make) {
    const int64_t L = state.range(0);
    std::mt19937_64 rng(kSeed);
    std::uniform_int_distribution<int64_t> price(0, L - 1);
    std::uniform_int_distribution<int>     roll(0, 99);

    struct Op { int kind; Price p; }; // 0 insert, 1 erase, 2 best
    std::vector<Op> ops(kOps);
    for (auto& o : ops) {
        const int r = roll(rng);
        o.kind = r < 60 ? 0 : (r < 90 ? 1 : 2);
        o.p = price(rng);
    }

    for (auto _ : state) {
        auto c = make(L);
        for (const auto& o : ops) {
            switch (o.kind) {
                case 0: c.insert(o.p, PriceLevel(o.p)); break;
                case 1: c.erase(o.p); break;
                default: benchmark::DoNotOptimize(c.best()); break;
            }
        }
        benchmark::DoNotOptimize(&c);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kOps));
}

// (c) Read heavy. Preload L levels once, then run many best and find queries.
template <typename Make>
void read_heavy(benchmark::State& state, Make make) {
    const int64_t L = state.range(0);
    auto c = make(L);
    for (int64_t p = 0; p < L; ++p) c.insert(p, PriceLevel(p));

    std::mt19937_64 rng(kSeed);
    std::uniform_int_distribution<int64_t> dist(0, L - 1);
    std::vector<Price> qs(kOps);
    for (auto& q : qs) q = dist(rng);

    for (auto _ : state) {
        for (std::size_t i = 0; i < kOps; ++i) {
            if ((i & 7u) == 0) {
                benchmark::DoNotOptimize(c.best());
            } else {
                benchmark::DoNotOptimize(c.find(qs[i]));
            }
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kOps));
}

BENCHMARK_CAPTURE(insert_heavy, Map,    make_map)->Apply(args);
BENCHMARK_CAPTURE(insert_heavy, Vector, make_vec)->Apply(args);
BENCHMARK_CAPTURE(insert_heavy, Array,  make_arr)->Apply(args);

BENCHMARK_CAPTURE(mixed, Map,    make_map)->Apply(args);
BENCHMARK_CAPTURE(mixed, Vector, make_vec)->Apply(args);
BENCHMARK_CAPTURE(mixed, Array,  make_arr)->Apply(args);

BENCHMARK_CAPTURE(read_heavy, Map,    make_map)->Apply(args);
BENCHMARK_CAPTURE(read_heavy, Vector, make_vec)->Apply(args);
BENCHMARK_CAPTURE(read_heavy, Array,  make_arr)->Apply(args);
