#include "OrderBook.h"

OrderBook::OrderBook() = default;
OrderBook::~OrderBook() = default;
OrderBook::OrderBook(const OrderBook& other) = default;
OrderBook& OrderBook::operator=(const OrderBook& other) = default;
OrderBook::OrderBook(OrderBook&& other) noexcept = default;
OrderBook& OrderBook::operator=(OrderBook&& other) noexcept = default;

std::vector<std::shared_ptr<BookLevel>> OrderBook::getBuySideView(int numLevels) const {
    return buyLevels.getBookSideView(numLevels);
}

std::vector<std::shared_ptr<BookLevel>> OrderBook::getSellSideView(int numLevels) const {
    return sellLevels.getBookSideView(numLevels);
}

std::pair<std::vector<std::shared_ptr<BookLevel>>, std::vector<std::shared_ptr<BookLevel>>> OrderBook::getOrderBookView(int numLevels) const {
    return {buyLevels.getBookSideView(numLevels), sellLevels.getBookSideView(numLevels)};
}

int OrderBook::addOrder(double price, int quantity, OrderType type, OrderSide side) {
    int priceInt = static_cast<int>(price * PRICE_MULTIPLIER);
    int orderId = ++orderIdCounter;
    if (side == OrderSide::BUY) {
        buyLevels.addOrder(orderId, type, priceInt, quantity);
    } else {
        sellLevels.addOrder(orderId, type, priceInt, quantity);
    }
    return orderId;
}

bool OrderBook::cancelOrder(int orderId) { // This can be simplified by using a order manager class
    if (buyLevels.isOrderIdExist(orderId)) {
        return buyLevels.cancelOrder(orderId);
    }
    return sellLevels.cancelOrder(orderId);
}

bool OrderBook::modifyOrder(int orderId, int newQuantity, double newPrice, OrderSide newSide, OrderType type) { // This can be simplified by using a order manager class
    int newPriceInt = static_cast<int>(newPrice * PRICE_MULTIPLIER);
    bool isBuySide = buyLevels.isOrderIdExist(orderId);
    bool isSellSide = sellLevels.isOrderIdExist(orderId);
    if (!isBuySide && !isSellSide) {
        return false;
    }
    if (isBuySide) {
        if (newSide == OrderSide::BUY) {
            return buyLevels.modifyOrder(orderId, newQuantity, newPriceInt) != nullptr;
        }
        buyLevels.cancelOrder(orderId);
        return sellLevels.addOrder(orderId, type, newPriceInt, newQuantity) != nullptr;
    }
    if (newSide == OrderSide::SELL) {
        return sellLevels.modifyOrder(orderId, newQuantity, newPriceInt) != nullptr;
    }
    sellLevels.cancelOrder(orderId);
    return buyLevels.addOrder(orderId, type, newPriceInt, newQuantity) != nullptr;
}
