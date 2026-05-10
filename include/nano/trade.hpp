#pragma once

#include "nano/types.hpp"

namespace nano {

// A single execution. Every match between an incoming aggressor and a resting
// order produces one Trade. The trade always prints at the maker price, which
// is the price the resting order posted. The aggressor pays or receives price
// improvement relative to its own limit.
struct Trade {
    OrderId   maker_id   = INVALID_ORDER_ID;  // The resting order that was hit.
    OrderId   taker_id   = INVALID_ORDER_ID;  // The incoming aggressor.
    Price     price      = INVALID_PRICE;      // Always the maker price.
    Quantity  quantity   = 0;
    Side      taker_side = Side::Buy;           // Which way the aggressor traded.
    SymbolId  symbol     = INVALID_SYMBOL;
    Timestamp timestamp  = 0;
};

} // namespace nano
