#pragma once

#include "nano/message.hpp"
#include "nano/order_book.hpp"
#include "nano/trade.hpp"
#include "nano/types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nano {

// The engine routes messages to per instrument order books. It owns the shared
// order pool and the symbol table. Symbols are interned to integers once, at the
// edge, so the books and the matching path only ever compare integers.
class MatchingEngine {
public:
    explicit MatchingEngine(OrderPool& pool) : pool_(pool) {}

    // Process one inbound message and return the trades it produced.
    std::vector<Trade> process(const Message& msg);

    // Convenience wrappers for callers that already hold interned symbol ids.
    std::vector<Trade> submit(OrderId id, const std::string& symbol, Side side,
                              OrderType type, Price price, Quantity qty);
    bool cancel(const std::string& symbol, OrderId id);
    std::vector<Trade> modify(const std::string& symbol, OrderId id,
                              Price new_price, Quantity new_qty);

    // Look up a book. Creates it on first reference.
    OrderBook& book(const std::string& symbol);
    [[nodiscard]] const OrderBook* find_book(const std::string& symbol) const;

    [[nodiscard]] uint64_t total_messages_processed() const noexcept { return msg_count_; }
    [[nodiscard]] uint64_t total_trades()             const noexcept { return trade_count_; }
    [[nodiscard]] std::size_t instrument_count()      const noexcept { return books_.size(); }

    // Intern a symbol string to a stable integer id.
    SymbolId intern(const std::string& symbol);

private:
    OrderPool&                                pool_;
    std::unordered_map<std::string, OrderBook> books_;
    std::unordered_map<std::string, SymbolId>  symbol_ids_;
    SymbolId  next_symbol_id_ = 0;
    uint64_t  msg_count_      = 0;
    uint64_t  trade_count_    = 0;
    OrderId   auto_order_id_  = 1; // Used when a message omits an id.
};

} // namespace nano
