#include "Matcher.h"

Matcher::Matcher(std::shared_ptr<OrderBook>& ob): order_book(ob) {}

bool Matcher::tryMatch(order_id_t order_id) {
    OrderSide order_side = order_book->getOrderSide(order_id).value();
    OrderType order_type = order_book->getOrderType(order_id).value();
    int price = order_book->getOrderPrice(order_id).value();
    int quantity = order_book->getOrderQuantity(order_id).value();

    OrderSide side = (order_side==OrderSide::BUY) ? OrderSide::SELL : OrderSide::BUY;

    int rem_qty = order_book->fillOrders(side, price, quantity);

    order_book->modifyOrder(order_id, rem_qty, price, order_side, order_type);

    return (rem_qty==0);
}
