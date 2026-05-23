#include "nano/order_book.hpp"

#include <algorithm>

namespace nano {

namespace {

// Match an incoming aggressor against the opposite side of the book. The
// predicate decides whether the best opposite price is acceptable, which is how
// a limit price is enforced. A market order passes a predicate that always says
// yes. Fully filled resting orders are unlinked and returned to the pool.
template <typename BookSide, typename PriceOk>
NANO_HOT void match_against(Order* incoming, BookSide& opposite,
                            std::unordered_map<OrderId, Order*>& order_map,
                            OrderPool* pool, SymbolId symbol,
                            std::vector<Trade>& trades, PriceOk price_ok) {
    while (incoming->remaining_qty > 0 && NANO_LIKELY(!opposite.empty())) {
        auto it = opposite.begin();
        PriceLevel& level = it->second;
        if (!price_ok(level.price())) break; // Best price no longer crosses.

        while (!level.empty() && incoming->remaining_qty > 0) {
            Order* maker = level.front();
            const Quantity exec = std::min(maker->remaining_qty, incoming->remaining_qty);

            trades.push_back(Trade{maker->id, incoming->id, maker->price, exec,
                                   incoming->side, symbol, now()});

            maker->remaining_qty    -= exec;
            incoming->remaining_qty -= exec;
            level.reduce(exec);

            if (maker->remaining_qty == 0) {
                level.remove(maker);
                order_map.erase(maker->id);
                pool->deallocate(maker);
            }
        }
        if (level.empty()) opposite.erase(it);
    }
}

// Sum the liquidity available to an aggressor at acceptable prices. Used only by
// fill or kill, which must know before it trades whether the whole order can be
// satisfied.
template <typename BookSide, typename PriceOk>
Quantity available_liquidity(const BookSide& opposite, Quantity needed, PriceOk price_ok) {
    Quantity have = 0;
    for (const auto& [price, level] : opposite) {
        if (!price_ok(price)) break;
        have += level.total_quantity();
        if (have >= needed) return have;
    }
    return have;
}

} // namespace

std::vector<Trade> OrderBook::match(Order* incoming) {
    std::vector<Trade> trades;

    if (incoming->side == Side::Buy) {
        // A buy crosses asks priced at or below its limit. A market buy takes any.
        const Price limit = incoming->price;
        const bool is_market = incoming->type == OrderType::Market;
        match_against(incoming, asks_, order_map_, pool_, symbol_, trades,
                      [&](Price ask) { return is_market || ask <= limit; });
    } else {
        // A sell crosses bids priced at or above its limit. A market sell takes any.
        const Price limit = incoming->price;
        const bool is_market = incoming->type == OrderType::Market;
        match_against(incoming, bids_, order_map_, pool_, symbol_, trades,
                      [&](Price bid) { return is_market || bid >= limit; });
    }
    return trades;
}

bool OrderBook::can_fully_fill(const Order* order) const noexcept {
    const Quantity need = order->remaining_qty;
    if (order->side == Side::Buy) {
        const Price limit = order->price;
        const bool is_market = order->type == OrderType::Market;
        return available_liquidity(asks_, need,
                                   [&](Price ask) { return is_market || ask <= limit; }) >= need;
    }
    const Price limit = order->price;
    const bool is_market = order->type == OrderType::Market;
    return available_liquidity(bids_, need,
                               [&](Price bid) { return is_market || bid >= limit; }) >= need;
}

void OrderBook::rest(Order* order) {
    if (order->side == Side::Buy) {
        auto [it, inserted] = bids_.try_emplace(order->price, PriceLevel(order->price));
        it->second.append(order);
    } else {
        auto [it, inserted] = asks_.try_emplace(order->price, PriceLevel(order->price));
        it->second.append(order);
    }
    order_map_[order->id] = order;
}

std::vector<Trade> OrderBook::add_order(Order* order) {
    // Fill or kill must be all or nothing, so it checks before it touches anyone.
    if (NANO_UNLIKELY(order->type == OrderType::FillOrKill)) {
        if (!can_fully_fill(order)) {
            pool_->deallocate(order); // Rejected. Nothing printed, nothing rested.
            return {};
        }
    }

    std::vector<Trade> trades = match(order);

    // A limit order with quantity left over rests and keeps time priority from
    // this arrival. Everything else that is unfilled leaves the book.
    if (order->remaining_qty > 0 && order->type == OrderType::Limit) {
        rest(order);
    } else {
        pool_->deallocate(order);
    }
    return trades;
}

bool OrderBook::cancel_order(OrderId id) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return false;

    Order* order = it->second;
    if (order->side == Side::Buy) {
        auto lit = bids_.find(order->price);
        if (lit != bids_.end()) {
            lit->second.remove(order);
            if (lit->second.empty()) bids_.erase(lit);
        }
    } else {
        auto lit = asks_.find(order->price);
        if (lit != asks_.end()) {
            lit->second.remove(order);
            if (lit->second.empty()) asks_.erase(lit);
        }
    }
    order_map_.erase(it);
    pool_->deallocate(order);
    return true;
}

std::vector<Trade> OrderBook::modify_order(OrderId id, Quantity new_qty, Price new_price) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return {};

    // Remember the order's identity, then cancel and re submit at the new terms.
    // Re submitting means the order goes to the back of its new price level and
    // loses its old time priority, which is the standard exchange rule.
    const Order* old = it->second;
    const Side side = old->side;
    const OrderType type = old->type;
    const SymbolId sym = old->symbol;

    cancel_order(id);

    Order* fresh = pool_->allocate(id, side, type, new_price, new_qty, now(), sym);
    if (NANO_UNLIKELY(fresh == nullptr)) return {}; // Pool exhausted.
    return add_order(fresh);
}

Price OrderBook::best_bid() const noexcept {
    return bids_.empty() ? INVALID_PRICE : bids_.begin()->first;
}

Price OrderBook::best_ask() const noexcept {
    return asks_.empty() ? INVALID_PRICE : asks_.begin()->first;
}

Price OrderBook::spread() const noexcept {
    if (bids_.empty() || asks_.empty()) return 0;
    return asks_.begin()->first - bids_.begin()->first;
}

MarketDataSnapshot OrderBook::snapshot(uint32_t depth) const {
    MarketDataSnapshot snap;
    snap.symbol    = symbol_name_;
    snap.best_bid  = best_bid();
    snap.best_ask  = best_ask();
    snap.spread    = spread();
    snap.timestamp = now();

    snap.bids.reserve(depth);
    uint32_t n = 0;
    for (const auto& [price, level] : bids_) {
        if (n++ >= depth) break;
        snap.bids.push_back(DepthEntry{price, level.total_quantity(), level.order_count()});
    }
    snap.asks.reserve(depth);
    n = 0;
    for (const auto& [price, level] : asks_) {
        if (n++ >= depth) break;
        snap.asks.push_back(DepthEntry{price, level.total_quantity(), level.order_count()});
    }
    return snap;
}

} // namespace nano
