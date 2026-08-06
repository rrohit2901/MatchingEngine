#pragma once

#include <concepts>
#include <memory>
#include <utility>

#include "Events.h"

template<typename Cont, typename T>
concept validInputContConsumer = requires (Cont cont, T value) {
    {cont.push(std::move(value))} -> std::same_as<void>;
};


template<template<typename> class InputCont, typename T = std::unique_ptr<Event>>
requires validInputContConsumer<InputCont<T>, T>
class EventManager {
    private:
        std::shared_ptr<InputCont<T>> event_container;
    public:
        EventManager(std::shared_ptr<InputCont<T>> event_container): event_container(event_container) {};

        void addTradeEvent(order_id_t buy_order_id, order_id_t sell_order_id, int trade_price, int trade_qty) {
            event_container->push(std::make_unique<TradeEvent>(buy_order_id, sell_order_id, trade_price, trade_qty));
        }

        void addTradeEvent(const TradeEvent& trade_event) {
            event_container->push(std::make_unique<TradeEvent>(trade_event));
        }

        void addLimitOrderAddEvent(order_id_t order_id, int price, int qty, OrderSide side) {
            event_container->push(std::make_unique<LimitOrderAdd>(order_id, price, qty, side));
        }

        void addMarketOrderAddEvent(order_id_t order_id, int qty, OrderSide side) {
            event_container->push(std::make_unique<MarketOrderAdd>(order_id, qty, side));
        }

        void addOrderModifiedEvent(order_id_t order_id, order_id_t new_order_id, int new_price, int new_qty, OrderSide side) {
            event_container->push(std::make_unique<OrderModified>(order_id, new_order_id, new_price, new_qty, side));
        }

        void addOrderCancelledEvent(order_id_t order_id, OrderSide side) {
            event_container->push(std::make_unique<OrderCancelled>(order_id, side));
        }

        void addOrderRejectedEvent(int price, int qty, OrderSide side, OrderType type, std::string reason) {
            event_container->push(std::make_unique<OrderRejected>(price, qty, side, type, std::move(reason)));
        }

        void addOrderModifyRejectedEvent(order_id_t order_id, int new_price, int new_qty, OrderSide side, std::string reason) {
            event_container->push(std::make_unique<OrderModifyRejected>(order_id, new_price, new_qty, side, std::move(reason)));
        }

        void addSessionOpenEvent() {
            event_container->push(std::make_unique<SessionOpen>());
        }

        void addSessionCloseEvent() {
            event_container->push(std::make_unique<SessionClose>());
        }
};
