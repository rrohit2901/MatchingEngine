#include "Order.h"

Order::Order(order_id_t orderId, OrderSide side, OrderType type, int price, int quantity)
    : view{orderId, side, type, price, quantity}, isCancelled(false), isFulfilled(false) {}

Order::~Order() = default;
Order::Order(const Order& other)= default;
Order& Order::operator=(const Order& other) = default;

Order::Order(Order&& other) noexcept {
    view = other.view;
    isCancelled = other.isCancelled;
    isFulfilled = other.isFulfilled;

    other.isCancelled = true;
    other.isFulfilled = false;
}

Order& Order::operator=(Order&& other) noexcept {
    if (this != &other) {
        view = other.view;
        isCancelled = other.isCancelled;
        isFulfilled = other.isFulfilled;

        other.isCancelled = true;
        other.isFulfilled = false;
    }
    return *this;
}

OrderView Order::getView() const {
    return view;
}

OrderSide Order::getSide() const {
    return view.side;
}

OrderType Order::getType() const {
    return view.type;
}

int Order::getPrice() const {
    return view.price;
}

int Order::getQuantity() const {
    return view.quantity;
}

order_id_t Order::getOrderId() const {
    return view.orderId;
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
    if(qty>=view.quantity) {
        qty -= view.quantity;
        isFulfilled = true;
        return qty;
    }
    view.quantity -= qty;
    return 0;
}

bool Order::modify(int newQuantity, int newPrice) {
    if (!valid()) {
        return false;
    }
    view.quantity = newQuantity;
    view.price = newPrice;
    if(view.quantity==0) {
        isFulfilled = true;
    }
    return true;
}
