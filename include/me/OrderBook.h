#pragma once

#include "OrderBookSide.h"
#include "Order.h"
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

        std::vector<std::shared_ptr<BookLevel>> getBuySideView(int numLevels = 1) const;
        std::vector<std::shared_ptr<BookLevel>> getSellSideView(int numLevels = 1) const;
        std::pair<std::vector<std::shared_ptr<BookLevel>>, std::vector<std::shared_ptr<BookLevel>>> getOrderBookView(int numLevels = 1) const;

        order_id_t addOrder(int price, int quantity, OrderType type, OrderSide side);
        bool cancelOrder(order_id_t orderId);
        std::optional<order_id_t> modifyOrder(order_id_t orderId, int newQuantity, int newPrice, OrderSide newSide, OrderType type);
        int fillOrders(OrderSide side, int target_price, int qty);

        // Preferred accessor: one OrderManager lookup for every field.
        std::optional<OrderView> getOrderView(order_id_t order_id) const;

        std::optional<OrderSide> getOrderSide(order_id_t order_id) const;
        std::optional<OrderType> getOrderType(order_id_t order_id) const;
        std::optional<int> getOrderPrice(order_id_t order_id) const;
        std::optional<int> getOrderQuantity(order_id_t order_id) const;
        bool IsOrderValid(order_id_t order_id) const;
};
