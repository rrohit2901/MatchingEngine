#include "Order.h"

Order::Order(order_id_t order_id, OrderSide order_side, OrderType order_type, int order_price, int order_quantity)
    : orderId(order_id), price(order_price), quantity(order_quantity), side(order_side), type(order_type),
      isCancelled(false), isFulfilled(false) {}

Order::~Order() = default;
Order::Order(const Order& other)= default;
Order& Order::operator=(const Order& other) = default;

Order::Order(Order&& other) noexcept {
    orderId = other.orderId;
    price = other.price;
    quantity = other.quantity;
    side = other.side;
    type = other.type;
    isCancelled = other.isCancelled;
    isFulfilled = other.isFulfilled;

    other.isCancelled = true;
    other.isFulfilled = false;
}

Order& Order::operator=(Order&& other) noexcept {
    if (this != &other) {
        orderId = other.orderId;
        price = other.price;
        quantity = other.quantity;
        side = other.side;
        type = other.type;
        isCancelled = other.isCancelled;
        isFulfilled = other.isFulfilled;

        other.isCancelled = true;
        other.isFulfilled = false;
    }
    return *this;
}

OrderView Order::getView() const {
    return {orderId, side, type, price, quantity};
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

order_id_t Order::getOrderId() const {
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
    if (!valid()) [[unlikely]] {
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
    if(quantity==0) {
        isFulfilled = true;
    }
    return true;
}
