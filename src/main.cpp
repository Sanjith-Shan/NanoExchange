// NanoExchange demo and market simulation.
//
// This program drives the matching engine with a synthetic but realistic order
// flow and reports per operation latency at nanosecond granularity. It exists to
// show the engine working end to end and to produce the numbers quoted in the
// README and the design writeup.

#include "nano/matching_engine.hpp"
#include "nano/order_book.hpp"
#include "nano/measure.hpp"
#include "nano/stats.hpp"
#include "nano/types.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace nano;

namespace {

struct Config {
    uint64_t    orders  = 1'000'000;
    uint32_t    traders = 100;
    std::string symbol  = "AAPL";
    uint64_t    seed    = 42;
    int         pin     = -1;   // core to pin to, Linux only
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc) {
            c.orders = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--traders") == 0 && i + 1 < argc) {
            c.traders = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--symbol") == 0 && i + 1 < argc) {
            c.symbol = argv[++i];
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            c.seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
            c.pin = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage nano_exchange [--orders N] [--traders N] "
                        "[--symbol SYM] [--seed N] [--pin CORE]\n");
            std::exit(0);
        }
    }
    return c;
}

void print_book(const OrderBook& book, uint32_t depth) {
    MarketDataSnapshot snap = book.snapshot(depth);
    std::printf("\n  Book depth for %s   best bid %lld   best ask %lld   spread %lld\n",
                snap.symbol.c_str(),
                static_cast<long long>(snap.best_bid),
                static_cast<long long>(snap.best_ask),
                static_cast<long long>(snap.spread));
    std::printf("  %14s %10s      %14s %10s\n", "BID px", "qty", "ASK px", "qty");
    const std::size_t rows = std::max(snap.bids.size(), snap.asks.size());
    for (std::size_t i = 0; i < rows; ++i) {
        char bid[48] = "                         ";
        char ask[48] = "";
        if (i < snap.bids.size()) {
            std::snprintf(bid, sizeof(bid), "%14lld %10u",
                          static_cast<long long>(snap.bids[i].price), snap.bids[i].quantity);
        }
        if (i < snap.asks.size()) {
            std::snprintf(ask, sizeof(ask), "%14lld %10u",
                          static_cast<long long>(snap.asks[i].price), snap.asks[i].quantity);
        }
        std::printf("  %-25s   %-25s\n", bid, ask);
    }
}

} // namespace

int main(int argc, char** argv) {
    const Config cfg = parse_args(argc, argv);

    // Pin before anything is measured, and report what was actually achieved
    // rather than what was asked for. A table that says "pinned" above a run
    // that could not pin is worse than one that says nothing.
    PinningReport pin;
    if (cfg.pin >= 0) {
        pin = pin_to_core(cfg.pin);
        if (!pin.achieved) {
            std::fprintf(stderr, "could not pin to core %d: %s\n", cfg.pin, pin.reason);
        }
    }
    BoxInfo box = detect_box();
    box.pinned  = pin.achieved;

    std::printf("Machine   %s\n", box.one_line().c_str());
    if (pin.requested) {
        std::printf("Pinning   %s", pin.achieved ? "achieved" : pin.reason);
        if (pin.achieved) std::printf(", running on core %d", current_core());
        std::printf("\n");
    } else if (pinning_supported()) {
        std::printf("Pinning   not requested. Pass --pin CORE for a tail worth reading\n");
    } else {
        std::printf("Pinning   unavailable on this platform, so the median is the honest\n"
                    "          measure here and the tail is scheduler noise\n");
    }

    std::printf("NanoExchange simulation\n");
    std::printf("  orders  %llu\n", static_cast<unsigned long long>(cfg.orders));
    std::printf("  traders %u\n", cfg.traders);
    std::printf("  symbol  %s\n", cfg.symbol.c_str());
    std::printf("  seed    %llu\n\n", static_cast<unsigned long long>(cfg.seed));

    OrderPool      pool;
    MatchingEngine engine(pool);

    // Latency collectors, one per operation kind.
    LatencyStats add_limit, add_market, cancel_stats, modify_stats, bestquote, snap_stats;
    add_limit.reserve(cfg.orders);
    cancel_stats.reserve(cfg.orders / 4);

    std::mt19937_64 rng(cfg.seed);
    std::uniform_real_distribution<double> action(0.0, 1.0);
    std::normal_distribution<double>       offset(0.0, 40.0); // Price noise in ticks.
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);
    std::uniform_int_distribution<int>      side_dist(0, 1);
    std::uniform_int_distribution<int>      drift(-3, 3);

    Price mid = 100'000; // Starting mid price in ticks.
    OrderId next_id = 1;
    std::vector<OrderId> live; // Order ids believed to be resting on the book.
    live.reserve(cfg.orders / 2);

    uint64_t trades_done = 0;
    const Timestamp wall_start = now();

    for (uint64_t i = 0; i < cfg.orders; ++i) {
        const double roll = action(rng);

        if (roll < 0.20 && !live.empty()) {
            // Cancel a random tracked order. Some tracked ids have already been
            // consumed as makers, so we only time and record real cancels of
            // orders still resting on the book. This keeps the Cancel numbers
            // honest rather than dominated by fast misses.
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t idx = pick(rng);
            const OrderId id = live[idx];
            live[idx] = live.back();
            live.pop_back();

            if (engine.book(cfg.symbol).contains(id)) {
                const Timestamp t0 = now();
                engine.cancel(cfg.symbol, id);
                cancel_stats.record(now() - t0);
            }

        } else if (roll < 0.30 && !live.empty()) {
            // Modify a random resting order to a fresh price and size.
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t idx = pick(rng);
            const OrderId id = live[idx];
            const Price new_price = mid + static_cast<Price>(offset(rng));
            const Quantity new_qty = qty_dist(rng);

            const Timestamp t0 = now();
            const auto trades = engine.modify(cfg.symbol, id, new_price, new_qty);
            modify_stats.record(now() - t0);
            trades_done += trades.size();
            // The order keeps its id and may still rest, so it stays in live.

        } else {
            // New order. Ten percent are market orders, the rest are limits.
            const Side side = side_dist(rng) ? Side::Buy : Side::Sell;
            const bool is_market = action(rng) < 0.10;
            const Quantity qty = qty_dist(rng);
            const OrderId id = next_id++;

            Price price;
            OrderType type;
            if (is_market) {
                type = OrderType::Market;
                price = INVALID_PRICE;
            } else {
                type = OrderType::Limit;
                const Price noise = static_cast<Price>(offset(rng));
                // Buys sit a touch below mid, sells a touch above, plus noise.
                price = side == Side::Buy ? mid - 5 + noise : mid + 5 + noise;
                if (price < 1) price = 1;
            }

            const Timestamp t0 = now();
            const auto trades = engine.submit(id, cfg.symbol, side, type, price, qty);
            const Timestamp dt = now() - t0;
            if (is_market) add_market.record(dt);
            else           add_limit.record(dt);
            trades_done += trades.size();

            if (type == OrderType::Limit && engine.book(cfg.symbol).contains(id)) {
                live.push_back(id);
            }
        }

        // Random walk the mid so the book keeps moving.
        mid += drift(rng);
        if (mid < 100) mid = 100;

        // Periodically sample read side latency and print a heartbeat.
        if ((i + 1) % 100'000 == 0) {
            const OrderBook& b = engine.book(cfg.symbol);
            const Timestamp q0 = now();
            volatile Price bb = b.best_bid();
            volatile Price ba = b.best_ask();
            (void)bb; (void)ba;
            bestquote.record(now() - q0);

            const Timestamp s0 = now();
            volatile auto snap = b.snapshot(5);
            (void)snap;
            snap_stats.record(now() - s0);

            std::printf("[%9llu msgs]  best bid %lld  best ask %lld  "
                        "trades %llu  resting %zu  levels %u/%u\n",
                        static_cast<unsigned long long>(i + 1),
                        static_cast<long long>(b.best_bid()),
                        static_cast<long long>(b.best_ask()),
                        static_cast<unsigned long long>(trades_done),
                        static_cast<std::size_t>(b.total_orders()),
                        b.bid_levels(), b.ask_levels());
        }
    }

    const Timestamp wall_end = now();
    const double seconds = static_cast<double>(wall_end - wall_start) / 1e9;
    const double msg_per_sec = static_cast<double>(cfg.orders) / seconds;

    print_book(engine.book(cfg.symbol), 5);

    std::printf("\nLatency by operation\n\n");
    LatencyStats::print_header();
    add_limit.print("AddLimit");
    add_market.print("AddMarket");
    cancel_stats.print("Cancel");
    modify_stats.print("Modify");
    bestquote.print("BestBidAsk");
    snap_stats.print("Snapshot(5)");

    std::printf("\nSummary\n");
    std::printf("  messages processed   %llu\n",
                static_cast<unsigned long long>(engine.total_messages_processed()));
    std::printf("  trades executed      %llu\n",
                static_cast<unsigned long long>(trades_done));
    std::printf("  wall time            %.3f s\n", seconds);
    std::printf("  throughput           %.2f million msgs/sec\n", msg_per_sec / 1e6);

    if (!box.pinned) {
        std::printf("\n  This run was not pinned to a core, so the median is the honest\n"
                    "  measure and the tail is operating system scheduling noise. Say that\n"
                    "  before anyone else does.\n");
    } else if (!box.isolated) {
        std::printf("\n  Pinned but not isolated. No isolcpus on the kernel command line, so\n"
                    "  the core is this process's by preference and not by exclusion, and the\n"
                    "  far tail still contains whatever else the machine chose to run there.\n");
    }
    if (box.load_1min >= 0.0 && !box.quiet()) {
        std::printf("\n  The machine was busy while this ran. Treat every timing above as a\n"
                    "  lower bound on what the engine can do and rerun it on a quiet box.\n");
    }
    std::printf("  pool in use          %zu / %zu orders\n", pool.size(), pool.capacity());
    return 0;
}
