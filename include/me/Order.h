#pragma once

enum class OrderSide {
    BUY,
    SELL
};

enum class OrderType {
    LIMIT,
    MARKET
};

const double PRICE_MULTIPLIER = 10000; // To convert price to integer representation

class Order {
    private:
        OrderSide side;
        OrderType type;
        int price;
        int orderId;
        int quantity;
        bool isValid;
    public:
        Order(int orderId, OrderSide side, OrderType type, int price, int quantity);
        ~Order();
        Order(const Order&);
        Order& operator=(const Order&);
        Order(Order&&) noexcept;
        Order& operator=(Order&&) noexcept;

        OrderSide getSide() const;
        OrderType getType() const;
        int getPrice() const;
        int getQuantity() const;
        int getOrderId() const;
        bool valid() const;

        bool modify(int newQuantity, int newPrice);
        bool cancel();
};
