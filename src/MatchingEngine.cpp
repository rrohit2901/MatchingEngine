#include "MatchingEngine.h"

MatchingEngine::MatchingEngine(): matcher(order_book) {}

int MatchingEngine::addOrder(double price, int quantity, OrderType type, OrderSide side) {
    int order_id = order_book.addOrder(price, quantity, type, side);
    matcher.tryMatch(order_id);

    return order_id;
}

bool MatchingEngine::cancelOrder(int order_id) {
    return order_book.cancelOrder(order_id);
}

bool MatchingEngine::modifyOrder(int orderId, int newQuantity, double newPrice, OrderSide newSide, OrderType type) {
    return order_book.modifyOrder(orderId, newQuantity, newPrice, newSide, type);
}
