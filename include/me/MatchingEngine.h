#pragma once

#include "Matcher.h"

class MatchingEngine {
    private:
        std::shared_ptr<OrderBook> order_book;
        Matcher matcher;
    public:
        MatchingEngine();

        std::vector<std::shared_ptr<BookLevel>> getBuySideView(int numLevels = 1) const;
        std::vector<std::shared_ptr<BookLevel>> getSellSideView(int numLevels = 1) const;
        std::pair<std::vector<std::shared_ptr<BookLevel>>, std::vector<std::shared_ptr<BookLevel>>> getOrderBookView(int numLevels = 1) const;

        order_id_t addOrder(int price, int quantity, OrderType type, OrderSide side);
        bool cancelOrder(order_id_t orderId);
        std::optional<order_id_t> modifyOrder(order_id_t orderId, int newQuantity, int newPrice, OrderSide newSide, OrderType type);
};
