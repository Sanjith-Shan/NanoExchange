#pragma once

#include "nano/types.hpp"
#include <string>
#include <variant>

namespace nano {

// Inbound requests. These carry a human readable symbol string because they sit
// at the edge of the system where orders arrive from clients. The matching
// engine interns the symbol into a SymbolId once, then never touches the string
// again on the hot path.

struct NewOrderMessage {
    OrderId     id;
    std::string symbol;
    Side        side;
    OrderType   type;
    Price       price;     // Ignored for Market orders.
    Quantity    quantity;
};

struct CancelMessage {
    OrderId     id;
    std::string symbol;
};

struct ModifyMessage {
    OrderId     id;
    std::string symbol;
    Price       new_price;
    Quantity    new_quantity;
};

using Message = std::variant<NewOrderMessage, CancelMessage, ModifyMessage>;

} // namespace nano
