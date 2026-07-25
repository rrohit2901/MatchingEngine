#pragma once

#include "Matcher.h"

class MatchingEngine {
    private:
        OrderBook order_book;
        Matcher matcher;
    public:
        MatchingEngine();

        int addOrder(double price, int quantity, OrderType type, OrderSide side);
        bool cancelOrder(int orderId);
        bool modifyOrder(int orderId, int newQuantity, double newPrice, OrderSide newSide, OrderType type);
};
