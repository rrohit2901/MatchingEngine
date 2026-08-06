#pragma once

#include "Matcher.h"
#include "EventManager.h"
#include "RiskManager.h"

template<template<typename> class InputCont, typename T = std::unique_ptr<Event>>
requires validInputContConsumer<InputCont<T>, T>
class MatchingEngine {
    private:
        // Declaration order matters: matcher is constructed from the two above it.
        std::shared_ptr<OrderBook> order_book;
        std::shared_ptr<EventManager<InputCont, T>> event_manager;
        Matcher<InputCont, T> matcher;
        RiskManager risk_manager;
    public:
        MatchingEngine(std::shared_ptr<InputCont<T>> event_container, RiskParams risk_params);
        ~MatchingEngine();

        MatchingEngine(const MatchingEngine&) = delete;
        MatchingEngine& operator=(const MatchingEngine&) = delete;

        std::vector<std::shared_ptr<BookLevel>> getBuySideView(int numLevels = 1) const;
        std::vector<std::shared_ptr<BookLevel>> getSellSideView(int numLevels = 1) const;
        std::pair<std::vector<std::shared_ptr<BookLevel>>, std::vector<std::shared_ptr<BookLevel>>> getOrderBookView(int numLevels = 1) const;

        std::optional<order_id_t> addOrder(int price, int quantity, OrderType type, OrderSide side);
        bool cancelOrder(order_id_t orderId);
        std::optional<order_id_t> modifyOrder(order_id_t orderId, int newQuantity, int newPrice, OrderSide newSide, OrderType type);
};

// The session markers bracket the log: opening in the constructor and closing in
// the destructor guarantees every event in between is enclosed by a pair.
template<template<typename> class InputCont, typename T>
requires validInputContConsumer<InputCont<T>, T>
MatchingEngine<InputCont, T>::MatchingEngine(std::shared_ptr<InputCont<T>> event_container, RiskParams risk_params)
    : order_book{std::make_shared<OrderBook>()},
      event_manager{std::make_shared<EventManager<InputCont, T>>(event_container)},
      matcher{order_book, event_manager},
      risk_manager{risk_params} {
    event_manager->addSessionOpenEvent();
}

template<template<typename> class InputCont, typename T>
requires validInputContConsumer<InputCont<T>, T>
MatchingEngine<InputCont, T>::~MatchingEngine() {
    event_manager->addSessionCloseEvent();
}

template<template<typename> class InputCont, typename T>
requires validInputContConsumer<InputCont<T>, T>
std::optional<order_id_t> MatchingEngine<InputCont, T>::addOrder(int price, int quantity, OrderType type, OrderSide side) {
    std::optional<int> top_book_price = std::nullopt;
    const auto& book_top = side==OrderSide::BUY ? order_book->getBuySideView(1) : order_book->getSellSideView(1);
    if(!book_top.empty() && book_top[0]) top_book_price = book_top[0]->getPrice();
    bool is_valid = risk_manager(price, quantity, top_book_price);

    if(!is_valid) return std::nullopt;

    order_id_t order_id = order_book->addOrder(price, quantity, type, side);

    if (type == OrderType::MARKET) {
        event_manager->addMarketOrderAddEvent(order_id, quantity, side);
    } else {
        event_manager->addLimitOrderAddEvent(order_id, price, quantity, side);
    }

    matcher.tryMatch(order_id);

    return order_id;
}

template<template<typename> class InputCont, typename T>
requires validInputContConsumer<InputCont<T>, T>
bool MatchingEngine<InputCont, T>::cancelOrder(order_id_t orderId) {
    // The side has to be read before the cancel: a cancelled order is erased
    // from the OrderManager, so afterwards there is nothing left to ask.
    const auto order = order_book->getOrderView(orderId);

    bool is_cancelled = order_book->cancelOrder(orderId);
    if (is_cancelled && order) {
        event_manager->addOrderCancelledEvent(orderId, order->side);
    }
    return is_cancelled;
}

template<template<typename> class InputCont, typename T>
requires validInputContConsumer<InputCont<T>, T>
std::optional<order_id_t> MatchingEngine<InputCont, T>::modifyOrder(order_id_t orderId, int newQuantity, int newPrice, OrderSide newSide, OrderType type) {
    std::optional<int> top_book_price = std::nullopt;
    const auto& book_top = side==OrderSide::BUY ? order_book->getBuySideView(1) : order_book->getSellSideView(1);
    if(!book_top.empty() && book_top[0]) top_book_price = book_top[0]->getPrice();
    bool is_valid = risk_manager(price, quantity, top_book_price);

    if(!is_valid) return std::nullopt;
    
    auto modified_order_id = order_book->modifyOrder(orderId, newQuantity, newPrice, newSide, type);
    if (modified_order_id) {
        // Both ids are logged because a reprice retires the original order and
        // books a replacement under a fresh id.
        event_manager->addOrderModifiedEvent(orderId, modified_order_id.value(), newPrice, newQuantity, newSide);
        matcher.tryMatch(modified_order_id.value());
    }
    return modified_order_id;
}

template<template<typename> class InputCont, typename T>
requires validInputContConsumer<InputCont<T>, T>
std::vector<std::shared_ptr<BookLevel>> MatchingEngine<InputCont, T>::getBuySideView(int numLevels) const {
    return order_book->getBuySideView(numLevels);
}

template<template<typename> class InputCont, typename T>
requires validInputContConsumer<InputCont<T>, T>
std::vector<std::shared_ptr<BookLevel>> MatchingEngine<InputCont, T>::getSellSideView(int numLevels) const {
    return order_book->getSellSideView(numLevels);
}

template<template<typename> class InputCont, typename T>
requires validInputContConsumer<InputCont<T>, T>
std::pair<std::vector<std::shared_ptr<BookLevel>>, std::vector<std::shared_ptr<BookLevel>>> MatchingEngine<InputCont, T>::getOrderBookView(int numLevels) const {
    return {order_book->getBuySideView(numLevels), order_book->getSellSideView(numLevels)};
}
