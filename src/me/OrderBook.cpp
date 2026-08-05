#include "OrderBook.h"

OrderBook::OrderBook(): order_manager{std::make_shared<OrderManager>()}, buyLevels{order_manager}, sellLevels{order_manager} {};
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

order_id_t OrderBook::addOrder(int price, int quantity, OrderType type, OrderSide side) {
    order_id_t order_id;
    if (side == OrderSide::BUY) {
        order_id = buyLevels.addOrder(type, price, quantity);
    } else {
        order_id = sellLevels.addOrder(type, price, quantity);
    }
    return order_id;
}

bool OrderBook::cancelOrder(order_id_t orderId) { 
    if (buyLevels.isOrderIdExist(orderId)) {
        return buyLevels.cancelOrder(orderId);
    }
    return sellLevels.cancelOrder(orderId);
}

std::optional<order_id_t> OrderBook::modifyOrder(order_id_t orderId, int newQuantity, int newPrice, OrderSide newSide, OrderType type) { 
    bool isBuySide = buyLevels.isOrderIdExist(orderId);
    bool isSellSide = sellLevels.isOrderIdExist(orderId);
    if (!isBuySide && !isSellSide) {
        return std::nullopt;
    }
    if (isBuySide) {
        if (newSide == OrderSide::BUY) {
            return buyLevels.modifyOrder(orderId, newQuantity, newPrice);
        }
        buyLevels.cancelOrder(orderId);
        return sellLevels.addOrder(type, newPrice, newQuantity);
    }
    if (newSide == OrderSide::SELL) {
        return sellLevels.modifyOrder(orderId, newQuantity, newPrice);
    }
    sellLevels.cancelOrder(orderId);
    return buyLevels.addOrder(type, newPrice, newQuantity);
}

int OrderBook::fillOrders(OrderSide side, int target_price, int qty, std::vector<TradeEvent>& filled_orders, order_id_t counter_order_id) {
    if (side==OrderSide::BUY) {
        return buyLevels.fillOrders(target_price, qty, filled_orders, counter_order_id);
    }
    return sellLevels.fillOrders(target_price, qty, filled_orders, counter_order_id);
}

std::optional<OrderView> OrderBook::getOrderView(order_id_t order_id) const {
    return order_manager->getView(order_id);
}

// The single-field accessors below stay for callers that genuinely want one
// field. Anything reading two or more should use getOrderView().
std::optional<OrderSide> OrderBook::getOrderSide(order_id_t order_id) const {
    if (const auto order = order_manager->getView(order_id)) return order->side;
    return std::nullopt;
}

std::optional<OrderType> OrderBook::getOrderType(order_id_t order_id) const {
    if (const auto order = order_manager->getView(order_id)) return order->type;
    return std::nullopt;
}

std::optional<int> OrderBook::getOrderPrice(order_id_t order_id) const {
    if (const auto order = order_manager->getView(order_id)) return order->price;
    return std::nullopt;
}

std::optional<int> OrderBook::getOrderQuantity(order_id_t order_id) const {
    if (const auto order = order_manager->getView(order_id)) return order->quantity;
    return std::nullopt;
}

bool OrderBook::IsOrderValid(order_id_t order_id) const {
    return order_manager->valid(order_id);
}

