#pragma once

#include <cstdint>

// One byte each so OrderView packs into 20 bytes and an OrderManager slot fits
// in half a cache line.
enum class OrderSide : std::uint8_t {
    BUY,
    SELL
};

enum class OrderType : std::uint8_t {
    LIMIT,
    MARKET
};

// 64 bits: OrderManager packs a slot index (low 32) and a generation (high 32)
// into each id. See OrderManager.h.
using order_id_t = std::uint64_t;

// Immutable snapshot of everything a reader needs about an order. Fetching one
// costs a single OrderManager lookup, where the old per-field accessors cost one
// lookup each.
//
// The lifecycle flags (cancelled / fulfilled) are deliberately absent: an order
// is released by the OrderManager the moment it dies, so a view only ever
// describes a live order and the absence of a view is what signals "gone".
struct OrderView {
    order_id_t orderId;
    OrderSide side;
    OrderType type;
    int price;
    int quantity;
};

class Order {
    private:
        // Stored flat rather than as an embedded OrderView: OrderView rounds up
        // to 24 bytes, which would push the two flags into a fourth word. Flat,
        // the whole Order is 24 bytes and an OrderManager slot stays at 32.
        order_id_t orderId;
        int price;
        int quantity;
        OrderSide side;
        OrderType type;
        bool isCancelled;
        bool isFulfilled;
    public:
        Order(order_id_t orderId, OrderSide side, OrderType type, int price, int quantity);
        ~Order();
        Order(const Order&);
        Order& operator=(const Order&);
        Order(Order&&) noexcept;
        Order& operator=(Order&&) noexcept;

        // Returned by value: handing out a reference would let callers mutate the
        // state Order exists to guard.
        OrderView getView() const;

        OrderSide getSide() const;
        OrderType getType() const;
        int getPrice() const;
        int getQuantity() const;
        order_id_t getOrderId() const;
        bool valid() const;

        int fulfill(int qty);
        bool modify(int newQuantity, int newPrice);
        bool cancel();
};
