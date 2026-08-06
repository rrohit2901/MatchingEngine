#pragma once

#include <cstdlib>
#include <memory>
#include <optional>

#include "OrderBook.h"
#include "Order.h"

struct RiskParams{
    int max_allowed_quantity_quote = 100000;
    int min_allowed_quantity_quote = 1;
    // Absolute price distance from the top of book, in the same units as price.
    int max_price_book_top_deviation = 1000;
};

// Checks implemented
class RiskManager {
    private:
        RiskParams risk_params;
    public:
        RiskManager(RiskParams risk_params): risk_params(risk_params) {}

        // An order is valid only if EVERY check passes, so the results are
        // combined with &=. Using |= made runAllChecks() unconditionally true.
        bool runAllChecks(int price, int qty, std::optional<int> top_book_price) const {
            bool is_valid = true;

            // Fat finger checks
            is_valid &= fatFingerCheck(price, qty, top_book_price);

            return is_valid;
        }

        bool fatFingerCheck(int price, int qty, std::optional<int> top_book_price) const {
            // Checks related to quantity
            if (qty>risk_params.max_allowed_quantity_quote || qty<risk_params.min_allowed_quantity_quote) {return false;}
            // Checks related to price in the order related to top of the book.
            if (top_book_price && std::abs(price - top_book_price.value()) > risk_params.max_price_book_top_deviation) {return false;}
            return true;
        }
};
