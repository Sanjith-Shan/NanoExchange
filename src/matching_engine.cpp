#include "nano/matching_engine.hpp"

#include <variant>

namespace nano {

SymbolId MatchingEngine::intern(const std::string& symbol) {
    auto [it, inserted] = symbol_ids_.try_emplace(symbol, next_symbol_id_);
    if (inserted) ++next_symbol_id_;
    return it->second;
}

OrderBook& MatchingEngine::book(const std::string& symbol) {
    auto it = books_.find(symbol);
    if (it != books_.end()) return it->second;

    const SymbolId sym = intern(symbol);
    auto [ins, ok] = books_.try_emplace(symbol, &pool_, sym);
    ins->second.set_symbol_name(symbol);
    return ins->second;
}

const OrderBook* MatchingEngine::find_book(const std::string& symbol) const {
    auto it = books_.find(symbol);
    return it == books_.end() ? nullptr : &it->second;
}

std::vector<Trade> MatchingEngine::submit(OrderId id, const std::string& symbol, Side side,
                                          OrderType type, Price price, Quantity qty) {
    ++msg_count_;
    OrderBook& b = book(symbol);
    const SymbolId sym = intern(symbol);

    Order* order = pool_.allocate(id, side, type, price, qty, now(), sym);
    if (order == nullptr) return {}; // Pool exhausted. Reject silently.

    std::vector<Trade> trades = b.add_order(order);
    trade_count_ += trades.size();
    return trades;
}

bool MatchingEngine::cancel(const std::string& symbol, OrderId id) {
    ++msg_count_;
    auto it = books_.find(symbol);
    if (it == books_.end()) return false;
    return it->second.cancel_order(id);
}

std::vector<Trade> MatchingEngine::modify(const std::string& symbol, OrderId id,
                                          Price new_price, Quantity new_qty) {
    ++msg_count_;
    auto it = books_.find(symbol);
    if (it == books_.end()) return {};
    std::vector<Trade> trades = it->second.modify_order(id, new_qty, new_price);
    trade_count_ += trades.size();
    return trades;
}

std::vector<Trade> MatchingEngine::process(const Message& msg) {
    return std::visit(
        [this](const auto& m) -> std::vector<Trade> {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, NewOrderMessage>) {
                const OrderId id = m.id == INVALID_ORDER_ID ? auto_order_id_++ : m.id;
                return submit(id, m.symbol, m.side, m.type, m.price, m.quantity);
            } else if constexpr (std::is_same_v<T, CancelMessage>) {
                cancel(m.symbol, m.id);
                return {};
            } else { // ModifyMessage
                return modify(m.symbol, m.id, m.new_price, m.new_quantity);
            }
        },
        msg);
}

} // namespace nano
