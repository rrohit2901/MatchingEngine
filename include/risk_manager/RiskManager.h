#pragma once

#include <iostream>
#include <memory>
#include <optional>

#include "OrderBook.h"
#include "Order.h"

struct RiskParams{
    int max_allowed_quantity_quote = 1e5;
    int min_allowed_quantity_quote = 1;
    int max_price_book_top_deviation = 1000;
};

// Checks implemented 
class RiskManager {
    private:
        RiskParams risk_params;
    public:
        RiskManager(RiskParams risk_params): risk_params(risk_params) {}

        bool runAllChecks(int price, int qty, std::optional<int> top_book_price) {
            bool is_valid = true;

            // Fat finger checks
            is_valid |= fatFingerCheck(price, qty, top_book_price);

            return is_valid;
        }

        bool fatFingerCheck(int price, int qty,  std::optional<int> top_book_price) {
            // Checks related to quantity
            if (qty>risk_params.max_allowed_quantity_quote || qty<risk_params.min_allowed_quantity_quote) {return false;}
            // Checks related to price in the order related to top of the book
            if (top_book_price && price>top_book_price.value()*risk_params.max_price_book_top_deviation) {return false;}
            return true;
        }
};
