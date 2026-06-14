#include "BookLevel.h"
#include "Order.h"

BookLevel::BookLevel() : totalQuantity(0), price(0) {}
BookLevel::BookLevel(int price) : totalQuantity(0), price(price) {}

BookLevel::~BookLevel() = default;
BookLevel::BookLevel(const BookLevel& other) = default;
BookLevel& BookLevel::operator=(const BookLevel& other) = default;
BookLevel::BookLevel(BookLevel&& other) noexcept = default;
BookLevel& BookLevel::operator=(BookLevel&& other) noexcept = default;

int BookLevel::getTotalQuantity() const {
    return totalQuantity;
}

const std::vector<std::shared_ptr<Order>>& BookLevel::getOrders() const {
    return orders;
}

double BookLevel::getPrice() const {
    return price/PRICE_MULTIPLIER;
}

std::shared_ptr<Order> BookLevel::addOrder(int orderId, OrderSide side, OrderType type, int price, int quantity) {
    auto order = std::make_shared<Order>(orderId, side, type, price, quantity);
    orders.emplace_back(order);
    totalQuantity += quantity;
    return orders.back();
}

std::shared_ptr<Order> BookLevel::modifyOrder(std::shared_ptr<Order>& order, int newQuantity, int newPrice) {
    if(newQuantity <= order->getQuantity()) {
        totalQuantity -= (order->getQuantity() - newQuantity);
        order->modify(newQuantity, newPrice);
        return order;
    }
    order->cancel();
    auto modifiedOrder = std::make_shared<Order>(order->getOrderId(), order->getSide(), order->getType(), newPrice, newQuantity);
    orders.emplace_back(modifiedOrder);
    totalQuantity += (newQuantity - order->getQuantity());

    return modifiedOrder;
}

bool BookLevel::cancelOrder(std::shared_ptr<Order>& order) {
    if(order->cancel()) {
        totalQuantity -= order->getQuantity();
        return true;
    }
    return false;
}
