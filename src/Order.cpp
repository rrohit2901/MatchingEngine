#include "Order.h"

Order::Order(int orderId, OrderSide side, OrderType type, int price, int quantity) 
    : orderId(orderId), side(side), type(type), price(price), quantity(quantity), isCancelled(false), isFulfilled(false) {}

Order::~Order() = default;
Order::Order(const Order& other)= default;
Order& Order::operator=(const Order& other) = default;

Order::Order(Order&& other) noexcept {
    orderId = other.orderId;
    side = other.side;
    type = other.type;
    price = other.price;
    quantity = other.quantity;
    isCancelled = other.isCancelled;
    isFulfilled = other.isFulfilled;

    other.isCancelled = true; 
    other.isFulfilled = false;
}

Order& Order::operator=(Order&& other) noexcept {
    if (this != &other) {
        orderId = other.orderId;
        side = other.side;
        type = other.type;
        price = other.price;
        quantity = other.quantity;
        isCancelled = other.isCancelled;
        isFulfilled = other.isFulfilled;

        other.isCancelled = true; 
        other.isFulfilled = false;
    }
    return *this;
}

OrderSide Order::getSide() const {
    return side;
}

OrderType Order::getType() const {
    return type;
}

int Order::getPrice() const {
    return price;
}

int Order::getQuantity() const {
    return quantity;
}

int Order::getOrderId() const {
    return orderId;
}

bool Order::valid() const {
    return !(isCancelled || isFulfilled);
}

bool Order::cancel() {
    if (!valid()) {
        return false;
    }
    isCancelled = true;
    return true;
}

int Order::fulfill(int qty) {
    if (!valid()) {
        return qty;
    }
    if(qty>=quantity) {
        qty -= quantity;
        isFulfilled = true;
        return qty;
    }
    quantity -= qty;
    return 0;
}

bool Order::modify(int newQuantity, int newPrice) {
    if (!valid()) {
        return false;
    }
    quantity = newQuantity;
    price = newPrice;
    return true;
}
