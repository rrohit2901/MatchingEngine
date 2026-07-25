#include "Matcher.h"

Matcher::Matcher(OrderBook& order_book): order_book(order_book) {}

bool Matcher::tryMatch(int order_id) {
    std::shared_ptr<Order> order = order_book.getOrder(order_id);
    OrderSide side = order->getSide()==OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
    int rem_qty = order_book.fillOrders(side, order->getPrice(), order->getQuantity());
    order->fulfill(order->getQuantity() - rem_qty);
    return (rem_qty==0);
}
