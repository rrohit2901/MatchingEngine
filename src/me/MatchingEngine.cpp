#include "MatchingEngine.h"

MatchingEngine::MatchingEngine(): order_book{std::make_shared<OrderBook>()}, matcher{order_book} {}

order_id_t MatchingEngine::addOrder(int price, int quantity, OrderType type, OrderSide side) {
    order_id_t order_id = order_book->addOrder(price, quantity, type, side);
    matcher.tryMatch(order_id);

    return order_id;
}

bool MatchingEngine::cancelOrder(order_id_t order_id) {
    return order_book->cancelOrder(order_id);
}

std::optional<order_id_t> MatchingEngine::modifyOrder(order_id_t orderId, int newQuantity, int newPrice, OrderSide newSide, OrderType type) {
    auto modified_order_id =  order_book->modifyOrder(orderId, newQuantity, newPrice, newSide, type);
    if(modified_order_id)
        matcher.tryMatch(modified_order_id.value());
    return modified_order_id;
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
