#include "Order.h"

Order::Order(int orderId, OrderSide side, OrderType type, int price, int quantity) 
    : orderId(orderId), side(side), type(type), price(price), quantity(quantity), isValid(true) {}

Order::~Order() = default;
Order::Order(const Order& other)= default;
Order& Order::operator=(const Order& other) = default;

Order::Order(Order&& other) noexcept {
    orderId = other.orderId;
    side = other.side;
    type = other.type;
    price = other.price;
    quantity = other.quantity;
    isValid = other.isValid;

    other.isValid = false; 
}
Order& Order::operator=(Order&& other) noexcept {
    if (this != &other) {
        orderId = other.orderId;
        side = other.side;
        type = other.type;
        price = other.price;
        quantity = other.quantity;
        isValid = other.isValid;

        other.isValid = false; 
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
    return isValid;
}

bool Order::cancel() {
    if (!isValid) {
        return false;
    }
    isValid = false;
    return true;
}

bool Order::modify(int newQuantity, int newPrice) {
    if (!isValid) {
        return false;
    }
    quantity = newQuantity;
    price = newPrice;
    return true;
}
