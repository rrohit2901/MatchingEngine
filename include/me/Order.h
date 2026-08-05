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

class Order {
    private:
        OrderSide side;
        OrderType type;
        int price;
        int orderId;
        int quantity;
        bool isCancelled;
        bool isFulfilled;
    public:
        Order(order_id_t orderId, OrderSide side, OrderType type, int price, int quantity);
        ~Order();
        Order(const Order&);
        Order& operator=(const Order&);
        Order(Order&&) noexcept;
        Order& operator=(Order&&) noexcept;

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
