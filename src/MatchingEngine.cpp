#include "MatchingEngine.h"

MatchingEngine::MatchingEngine(): order_book{std::make_shared<OrderBook>()}, matcher{order_book} {}

int MatchingEngine::addOrder(double price, int quantity, OrderType type, OrderSide side) {
    int order_id = order_book->addOrder(price, quantity, type, side);
    matcher.tryMatch(order_id);

    return order_id;
}

bool MatchingEngine::cancelOrder(int order_id) {
    return order_book->cancelOrder(order_id);
}

bool MatchingEngine::modifyOrder(int orderId, int newQuantity, double newPrice, OrderSide newSide, OrderType type) {
    bool done =  order_book->modifyOrder(orderId, newQuantity, newPrice, newSide, type);
    matcher.tryMatch(orderId);
    return done;
}

std::vector<std::shared_ptr<BookLevel>> MatchingEngine::getBuySideView(int numLevels) const {
    return order_book->getBuySideView(numLevels);
}

std::vector<std::shared_ptr<BookLevel>> MatchingEngine::getSellSideView(int numLevels) const {
    return order_book->getSellSideView(numLevels);
}

std::pair<std::vector<std::shared_ptr<BookLevel>>, std::vector<std::shared_ptr<BookLevel>>> MatchingEngine::getOrderBookView(int numLevels) const {
    return {order_book->getBuySideView(numLevels), order_book->getSellSideView(numLevels)};
}
