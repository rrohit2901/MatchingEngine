#include "Matcher.h"

Matcher::Matcher(std::shared_ptr<OrderBook>& ob): order_book(ob) {}

bool Matcher::tryMatch(int order_id) {
    std::shared_ptr<Order> order = order_book->getOrder(order_id);
    OrderSide side = order->getSide()==OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
    int rem_qty = order_book->fillOrders(side, order->getPrice(), order->getQuantity());
    int filled_qty = order->getQuantity() - rem_qty;

    order_book->fillOrder(order_id, filled_qty);

    return (rem_qty==0);
}
