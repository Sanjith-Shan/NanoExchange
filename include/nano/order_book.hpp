#pragma once

#include "nano/market_data.hpp"
#include "nano/memory_pool.hpp"
#include "nano/order.hpp"
#include "nano/price_level.hpp"
#include "nano/trade.hpp"
#include "nano/types.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace nano {

// The default order pool. One million orders is enough for a heavy simulation
// and costs roughly sixty megabytes, which is trivial on a modern machine.
using OrderPool = MemoryPool<Order>;

// A single instrument order book with a bid side and an ask side.
//
// The two sides are ordered maps keyed by price. The bid map sorts high to low
// and the ask map sorts low to high, so best() on either side is the first
// element. An unordered map gives O(1) lookup by order id for cancel and modify.
//
// The book borrows a memory pool rather than owning it. The matching engine owns
// one pool shared across every instrument, which keeps all orders in one
// contiguous arena regardless of symbol.
class OrderBook {
public:
    OrderBook() = default;
    explicit OrderBook(OrderPool* pool, SymbolId symbol = INVALID_SYMBOL) noexcept
        : pool_(pool), symbol_(symbol) {}

    // Submit an already pooled order. Returns every trade it generated. If the
    // order is a limit with quantity left over it is added to the book and the
    // book keeps ownership. Otherwise the order is returned to the pool here.
    std::vector<Trade> add_order(Order* order);

    // Remove a resting order by id. Returns false if the id is not on the book.
    bool cancel_order(OrderId id);

    // Cancel then re add at the new price and quantity. This deliberately loses
    // time priority, which is the correct and standard exchange behaviour for a
    // price or size increase.
    std::vector<Trade> modify_order(OrderId id, Quantity new_qty, Price new_price);

    [[nodiscard]] Price best_bid() const noexcept;
    [[nodiscard]] Price best_ask() const noexcept;
    [[nodiscard]] Price spread() const noexcept;

    [[nodiscard]] MarketDataSnapshot snapshot(uint32_t depth = 5) const;

    [[nodiscard]] uint32_t bid_levels()   const noexcept { return static_cast<uint32_t>(bids_.size()); }
    [[nodiscard]] uint32_t ask_levels()   const noexcept { return static_cast<uint32_t>(asks_.size()); }
    [[nodiscard]] uint64_t total_orders() const noexcept { return order_map_.size(); }
    [[nodiscard]] bool     contains(OrderId id) const noexcept { return order_map_.count(id) != 0; }

    void set_symbol_name(std::string name) { symbol_name_ = std::move(name); }

private:
    // Match an aggressor against the opposite side. Fills the trades vector and
    // returns the quantity that was executed.
    std::vector<Trade> match(Order* incoming);

    // Rest a leftover limit order on its own side.
    void rest(Order* order);

    // Would the order fully fill against currently available liquidity? Used by
    // fill or kill to decide before touching the book.
    [[nodiscard]] bool can_fully_fill(const Order* order) const noexcept;

    OrderPool*  pool_   = nullptr;
    SymbolId    symbol_ = INVALID_SYMBOL;
    std::string symbol_name_;

    std::map<Price, PriceLevel, std::greater<Price>> bids_; // Best bid first.
    std::map<Price, PriceLevel>                      asks_; // Best ask first.
    std::unordered_map<OrderId, Order*>              order_map_;
};

} // namespace nano
