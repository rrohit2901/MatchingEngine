#pragma once

#include "OrderBookSide.h"
#include "Order.h"
#include<vector>
#include<memory>


class OrderBook {
    private:
        int orderIdCounter = 0;
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

        int addOrder(double price, int quantity, OrderType type, OrderSide side);
        bool cancelOrder(int orderId);
        bool modifyOrder(int orderId, int newQuantity, double newPrice, OrderSide newSide, OrderType type);
        Order getOrder(int orderId) const;
};
