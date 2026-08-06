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

// Why an order was turned away. NONE means it passed. A reject log that only
// says "rejected" cannot be acted on, so every check reports its own reason.
enum class RejectReason {
    NONE,
    QUANTITY_ABOVE_MAX,
    QUANTITY_BELOW_MIN,
    PRICE_TOO_FAR_FROM_TOP,
};

inline const char* to_string(RejectReason reason) {
    switch (reason) {
        case RejectReason::NONE:                   return "NONE";
        case RejectReason::QUANTITY_ABOVE_MAX:     return "QUANTITY_ABOVE_MAX";
        case RejectReason::QUANTITY_BELOW_MIN:     return "QUANTITY_BELOW_MIN";
        case RejectReason::PRICE_TOO_FAR_FROM_TOP: return "PRICE_TOO_FAR_FROM_TOP";
    }
    return "UNKNOWN";
}

// Checks implemented
class RiskManager {
    private:
        RiskParams risk_params;
    public:
        RiskManager(RiskParams risk_params): risk_params(risk_params) {}

        // Runs every check and reports the first reason to reject, or NONE.
        // An order is accepted only if EVERY check passes.
        RejectReason checkOrder(int price, int qty, std::optional<int> top_book_price) const {
            // Fat finger checks
            const RejectReason fat_finger = fatFingerCheck(price, qty, top_book_price);
            if (fat_finger != RejectReason::NONE) return fat_finger;

            return RejectReason::NONE;
        }

        RejectReason fatFingerCheck(int price, int qty, std::optional<int> top_book_price) const {
            // Checks related to quantity
            if (qty>risk_params.max_allowed_quantity_quote) {return RejectReason::QUANTITY_ABOVE_MAX;}
            if (qty<risk_params.min_allowed_quantity_quote) {return RejectReason::QUANTITY_BELOW_MIN;}
            // Checks related to price in the order related to top of the book.
            if (top_book_price && std::abs(price - top_book_price.value()) > risk_params.max_price_book_top_deviation) {return RejectReason::PRICE_TOO_FAR_FROM_TOP;}
            return RejectReason::NONE;
        }
};
