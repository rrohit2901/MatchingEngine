#pragma once

#include <concepts>
#include <memory>
#include <utility>
#include <variant>

#include "Events.h"

template<typename Cont, typename T>
concept validInputContConsumer = requires (Cont cont, T value) {
    {cont.push(std::move(value))} -> std::same_as<void>;
};


// Events are constructed in place inside the variant and pushed by value. The
// std::in_place_type form is what keeps that to a single copy: it builds the
// event directly in the variant's storage rather than materialising a temporary
// to move from. There is no allocation anywhere on this path -- that was the
// point of dropping unique_ptr<Event> from the queue.
template<template<typename> class InputCont, typename T = EventVariant>
requires validInputContConsumer<InputCont<T>, T>
class EventManager {
    private:
        std::shared_ptr<InputCont<T>> event_container;
    public:
        EventManager(std::shared_ptr<InputCont<T>> event_container): event_container(event_container) {};

        void addTradeEvent(order_id_t buy_order_id, order_id_t sell_order_id, int trade_price, int trade_qty) {
            event_container->push(T{std::in_place_type<TradeEvent>, buy_order_id, sell_order_id, trade_price, trade_qty});
        }

        void addTradeEvent(const TradeEvent& trade_event) {
            event_container->push(T{trade_event});
        }

        void addLimitOrderAddEvent(order_id_t order_id, int price, int qty, OrderSide side) {
            event_container->push(T{std::in_place_type<LimitOrderAdd>, order_id, price, qty, side});
        }

        void addMarketOrderAddEvent(order_id_t order_id, int qty, OrderSide side) {
            event_container->push(T{std::in_place_type<MarketOrderAdd>, order_id, qty, side});
        }

        void addOrderModifiedEvent(order_id_t order_id, order_id_t new_order_id, int new_price, int new_qty, OrderSide side) {
            event_container->push(T{std::in_place_type<OrderModified>, order_id, new_order_id, new_price, new_qty, side});
        }

        void addOrderCancelledEvent(order_id_t order_id, OrderSide side) {
            event_container->push(T{std::in_place_type<OrderCancelled>, order_id, side});
        }

        // `reason` must have static storage duration; see the contract on
        // OrderRejected. It is not copied.
        void addOrderRejectedEvent(int price, int qty, OrderSide side, OrderType type, const char* reason) {
            event_container->push(T{std::in_place_type<OrderRejected>, price, qty, side, type, reason});
        }

        void addOrderModifyRejectedEvent(order_id_t order_id, int new_price, int new_qty, OrderSide side, const char* reason) {
            event_container->push(T{std::in_place_type<OrderModifyRejected>, order_id, new_price, new_qty, side, reason});
        }

        void addSessionOpenEvent() {
            event_container->push(T{std::in_place_type<SessionOpen>});
        }

        void addSessionCloseEvent() {
            event_container->push(T{std::in_place_type<SessionClose>});
        }
};
