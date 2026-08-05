#pragma once

enum class OrderSide {
    BUY,
    SELL
};

enum class OrderType {
    LIMIT,
    MARKET
};

using order_id_t = unsigned int;

// Immutable snapshot of everything a reader needs about an order. Fetching one
// costs a single OrderManager lookup, where the old per-field accessors cost one
// lookup each.
//
// The lifecycle flags (cancelled / fulfilled) are deliberately absent: an order
// is erased from the OrderManager the moment it dies, so a view only ever
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
        OrderView view;
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
