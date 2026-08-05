#pragma once

#include "Matcher.h"
#include "EventManager.h"

template<template<typename> class InputCont, typename T = std::unique_ptr<Event>>
requires validInputContConsumer<InputCont<T>, T>
class MatchingEngine {
    private:
        std::shared_ptr<OrderBook> order_book;
        EventManager<InputCont, T> event_manager;
        Matcher matcher;
    public:
        MatchingEngine(std::shared_ptr<InputCont<T>> event_container);

        std::vector<std::shared_ptr<BookLevel>> getBuySideView(int numLevels = 1) const;
        std::vector<std::shared_ptr<BookLevel>> getSellSideView(int numLevels = 1) const;
        std::pair<std::vector<std::shared_ptr<BookLevel>>, std::vector<std::shared_ptr<BookLevel>>> getOrderBookView(int numLevels = 1) const;

        order_id_t addOrder(int price, int quantity, OrderType type, OrderSide side);
        bool cancelOrder(order_id_t orderId);
        std::optional<order_id_t> modifyOrder(order_id_t orderId, int newQuantity, int newPrice, OrderSide newSide, OrderType type);
};

template<template<typename> class InputCont, typename T>
MatchingEngine<InputCont, T>::MatchingEngine(std::shared_ptr<InputCont<T>> event_container): order_book{std::make_shared<OrderBook>()}, matcher{order_book}, event_manager{event_container} {}

template<template<typename> class InputCont, typename T>
order_id_t MatchingEngine<InputCont, T>::addOrder(int price, int quantity, OrderType type, OrderSide side) {
    order_id_t order_id = order_book->addOrder(price, quantity, type, side);
    event_manager.addLimitOrderAddEvent(order_id, price, quantity, side);
    matcher.tryMatch(order_id);

    return order_id;
}

template<template<typename> class InputCont, typename T>
bool MatchingEngine<InputCont, T>::cancelOrder(order_id_t order_id) {
    bool is_cancelled = order_book->cancelOrder(order_id);
    if(is_cancelled) event_manager.addOrderCancelledEvent(order_id, side);
}

template<template<typename> class InputCont, typename T>
std::optional<order_id_t> MatchingEngine<InputCont, T>::modifyOrder(order_id_t orderId, int newQuantity, int newPrice, OrderSide newSide, OrderType type) {
    auto modified_order_id =  order_book->modifyOrder(orderId, newQuantity, newPrice, newSide, type);
    event_manager.addOrderModifiedEvent(order_id, modified_order_id, newPrice, newQuantity);
    if(modified_order_id)
        matcher.tryMatch(modified_order_id.value());
    return modified_order_id;
}

template<template<typename> class InputCont, typename T>
std::vector<std::shared_ptr<BookLevel>> MatchingEngine<InputCont, T>::getBuySideView(int numLevels) const {
    return order_book->getBuySideView(numLevels);
}

template<template<typename> class InputCont, typename T>
std::vector<std::shared_ptr<BookLevel>> MatchingEngine<InputCont, T>::getSellSideView(int numLevels) const {
    return order_book->getSellSideView(numLevels);
}

template<template<typename> class InputCont, typename T>
std::pair<std::vector<std::shared_ptr<BookLevel>>, std::vector<std::shared_ptr<BookLevel>>> MatchingEngine<InputCont, T>::getOrderBookView(int numLevels) const {
    return {order_book->getBuySideView(numLevels), order_book->getSellSideView(numLevels)};
}
