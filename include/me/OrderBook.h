#pragma once

#include "OrderBookSide.h"
#include "Order.h"
#include "Events.h"
#include<vector>
#include<memory>


class OrderBook {
    private:
        std::shared_ptr<OrderManager> order_manager;
        OrderBookSide<OrderSide::BUY> buyLevels;
        OrderBookSide<OrderSide::SELL> sellLevels;
    public:
        OrderBook();
        ~OrderBook();
        OrderBook(const OrderBook&);
        OrderBook& operator=(const OrderBook&);
        OrderBook(OrderBook&&) noexcept;
        OrderBook& operator=(OrderBook&&) noexcept;

        std::vector<LevelView> getBuySideView(int numLevels = 1) const;
        std::vector<LevelView> getSellSideView(int numLevels = 1) const;
        std::pair<std::vector<LevelView>, std::vector<LevelView>> getOrderBookView(int numLevels = 1) const;

        order_id_t addOrder(int price, int quantity, OrderType type, OrderSide side);
        bool cancelOrder(order_id_t orderId);
        std::optional<order_id_t> modifyOrder(order_id_t orderId, int newQuantity, int newPrice, OrderSide newSide, OrderType type);
        int fillOrders(OrderSide side, int target_price, int qty, std::vector<TradeEvent>& filled_orders, order_id_t counter_order_id);

        // Preferred accessor: one OrderManager lookup for every field.
        std::optional<OrderView> getOrderView(order_id_t order_id) const;

        std::optional<int> getBestPrice(OrderSide side) const;
        // Best level on one side, or nullopt when that side is empty. No allocation.
        std::optional<LevelView> getTopLevel(OrderSide side) const;
        // Order ids resting at one price, front of the queue first. May contain
        // dead ids; valid only until the book next changes.
        std::span<const order_id_t> getLevelQueue(OrderSide side, int price) const;

        std::optional<OrderSide> getOrderSide(order_id_t order_id) const;
        std::optional<OrderType> getOrderType(order_id_t order_id) const;
        std::optional<int> getOrderPrice(order_id_t order_id) const;
        std::optional<int> getOrderQuantity(order_id_t order_id) const;
        bool IsOrderValid(order_id_t order_id) const;
};
