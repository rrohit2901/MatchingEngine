#pragma once

#include "OrderBook.h"
#include "Order.h"
#include "EventManager.h"
#include <vector>

template<template<typename> class InputCont, typename T = EventVariant>
requires validInputContConsumer<InputCont<T>, T>
class Matcher {
    private:
        std::shared_ptr<OrderBook> order_book;
        std::shared_ptr<EventManager<InputCont, T>> event_manager;
    public:
        Matcher(std::shared_ptr<OrderBook>& order_book, std::shared_ptr<EventManager<InputCont, T>>& event_manager);
        bool tryMatch(order_id_t order_id);
};

template<template<typename> class InputCont, typename T>
requires validInputContConsumer<InputCont<T>, T>
Matcher<InputCont, T>::Matcher(std::shared_ptr<OrderBook>& ob, std::shared_ptr<EventManager<InputCont, T>>& event_manager): order_book(ob), event_manager(event_manager) {}

template<template<typename> class InputCont, typename T>
requires validInputContConsumer<InputCont<T>, T>
bool Matcher<InputCont, T>::tryMatch(order_id_t order_id) {
    // One lookup for side, type, price and quantity; this used to be four.
    const auto order = order_book->getOrderView(order_id);
    if (!order) return false;

    OrderSide opposite = (order->side==OrderSide::BUY) ? OrderSide::SELL : OrderSide::BUY;

    std::vector<TradeEvent> filled_orders;
    int rem_qty = order_book->fillOrders(opposite, order->price, order->quantity, filled_orders, order_id);

    order_book->modifyOrder(order_id, rem_qty, order->price, order->side, order->type);

    for (TradeEvent& event: filled_orders) {
        event_manager->addTradeEvent(event);
    }

    return (rem_qty==0);
}

