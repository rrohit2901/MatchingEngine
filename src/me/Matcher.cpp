#include "Matcher.h"

Matcher::Matcher(std::shared_ptr<OrderBook>& ob): order_book(ob) {}

bool Matcher::tryMatch(order_id_t order_id) {
    // One lookup for side, type, price and quantity; this used to be four.
    const auto order = order_book->getOrderView(order_id);
    if (!order) return false;

    OrderSide opposite = (order->side==OrderSide::BUY) ? OrderSide::SELL : OrderSide::BUY;

    int rem_qty = order_book->fillOrders(opposite, order->price, order->quantity);

    order_book->modifyOrder(order_id, rem_qty, order->price, order->side, order->type);

    return (rem_qty==0);
}
