#pragma once

#include "OrderBook.h"
#include "Order.h"
#include <vector>

class Matcher {
    private:
        OrderBook order_book;
    public:
        Matcher(OrderBook& order_book);
        bool tryMatch(int order_id);
};
