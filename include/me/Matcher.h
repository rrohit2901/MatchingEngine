#pragma once

#include "OrderBook.h"
#include "Order.h"
#include <vector>

class Matcher {
    private:
        std::shared_ptr<OrderBook> order_book;
    public:
        Matcher(std::shared_ptr<OrderBook>& order_book);
        bool tryMatch(order_id_t order_id);
};
