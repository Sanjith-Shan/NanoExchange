#pragma once

#include "nano/types.hpp"
#include <type_traits>

namespace nano {

// A resting or incoming order.
//
// The prev and next pointers are intrusive. A price level threads its orders
// through these fields directly, so adding an order to a level costs zero heap
// allocation and zero extra indirection. When an Order sits on the memory pool
// free list instead of a price level, the pool reuses the same storage.
//
// Field order is chosen so the hot fields (side, type, price, remaining_qty)
// stay close together near the front of the object.
struct Order {
    OrderId  id            = INVALID_ORDER_ID;
    Price    price         = INVALID_PRICE;
    Quantity initial_qty   = 0;
    Quantity remaining_qty = 0;
    Side     side          = Side::Buy;
    OrderType type         = OrderType::Limit;
    bool     active        = false;   // True while allocated and live.
    SymbolId symbol        = INVALID_SYMBOL;
    Timestamp timestamp    = 0;        // Arrival time. Breaks ties by time priority.

    Order* prev = nullptr;            // Intrusive list neighbours within a price level.
    Order* next = nullptr;

    Order() = default;

    Order(OrderId id_, Side side_, OrderType type_, Price price_, Quantity qty_,
          Timestamp ts_, SymbolId sym_ = INVALID_SYMBOL) noexcept
        : id(id_), price(price_), initial_qty(qty_), remaining_qty(qty_),
          side(side_), type(type_), active(true), symbol(sym_), timestamp(ts_) {}

    [[nodiscard]] bool filled() const noexcept { return remaining_qty == 0; }
};

// Keep the object inside two cache lines. If this ever fails, split the cold
// fields (initial_qty, timestamp) into a side table.
static_assert(sizeof(Order) <= 128, "Order must fit within two cache lines");
static_assert(std::is_trivially_destructible_v<Order>,
              "Order must be trivially destructible for pool reuse");

} // namespace nano
