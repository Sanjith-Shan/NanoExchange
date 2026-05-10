#pragma once

#include "nano/types.hpp"
#include <string>
#include <vector>

namespace nano {

// One aggregated price level in a depth snapshot.
struct DepthEntry {
    Price    price       = INVALID_PRICE;
    Quantity quantity    = 0;   // Total resting quantity at this price.
    uint32_t order_count = 0;   // Number of resting orders at this price.
};

// A point in time view of the book. bids run from best (highest) downward and
// asks run from best (lowest) upward. This is what a market data feed publishes.
struct MarketDataSnapshot {
    std::string             symbol;
    Price                   best_bid = INVALID_PRICE;
    Price                   best_ask = INVALID_PRICE;
    Price                   spread   = 0;
    std::vector<DepthEntry> bids;
    std::vector<DepthEntry> asks;
    Timestamp               timestamp = 0;
};

} // namespace nano
