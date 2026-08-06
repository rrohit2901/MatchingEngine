#include "RiskManager.h"

#include <gtest/gtest.h>

#include <optional>

// RiskManager is a pure predicate over (price, quantity, top-of-book): it holds
// no state and touches no book, so every case can be driven directly.
//
// runAllChecks() must reject when ANY check rejects. The interesting cases are
// the boundaries, since an off-by-one there either lets a fat-finger order
// through or blocks legitimate flow.

namespace {

RiskParams defaults() {
    return RiskParams{};
}

} // namespace

TEST(RiskManager, AcceptsAnOrdinaryOrder) {
    RiskManager risk{defaults()};
    EXPECT_TRUE(risk.runAllChecks(100, 10, std::nullopt));
    EXPECT_TRUE(risk.runAllChecks(100, 10, 100));
}

TEST(RiskManager, RejectsQuantityAboveMax) {
    RiskParams params = defaults();
    RiskManager risk{params};

    EXPECT_TRUE(risk.runAllChecks(100, params.max_allowed_quantity_quote, std::nullopt));
    EXPECT_FALSE(risk.runAllChecks(100, params.max_allowed_quantity_quote + 1, std::nullopt));
}

TEST(RiskManager, RejectsQuantityBelowMin) {
    RiskParams params = defaults();
    RiskManager risk{params};

    EXPECT_TRUE(risk.runAllChecks(100, params.min_allowed_quantity_quote, std::nullopt));
    EXPECT_FALSE(risk.runAllChecks(100, params.min_allowed_quantity_quote - 1, std::nullopt));
}

TEST(RiskManager, RejectsZeroAndNegativeQuantity) {
    RiskManager risk{defaults()};
    EXPECT_FALSE(risk.runAllChecks(100, 0, std::nullopt));
    EXPECT_FALSE(risk.runAllChecks(100, -5, std::nullopt));
}

TEST(RiskManager, NoPriceCheckWhenBookIsEmpty) {
    RiskManager risk{defaults()};
    // With no top of book there is nothing to deviate from, so any price passes.
    EXPECT_TRUE(risk.runAllChecks(1, 10, std::nullopt));
    EXPECT_TRUE(risk.runAllChecks(99999999, 10, std::nullopt));
}

TEST(RiskManager, AcceptsPriceWithinDeviationOfTop) {
    RiskParams params = defaults();
    RiskManager risk{params};
    const int top = 5853300;

    EXPECT_TRUE(risk.runAllChecks(top, 10, top));
    EXPECT_TRUE(risk.runAllChecks(top + params.max_price_book_top_deviation, 10, top));
    EXPECT_TRUE(risk.runAllChecks(top - params.max_price_book_top_deviation, 10, top));
}

TEST(RiskManager, RejectsPriceTooFarAboveTop) {
    RiskParams params = defaults();
    RiskManager risk{params};
    const int top = 5853300;

    EXPECT_FALSE(risk.runAllChecks(top + params.max_price_book_top_deviation + 1, 10, top));
}

TEST(RiskManager, RejectsPriceTooFarBelowTop) {
    RiskParams params = defaults();
    RiskManager risk{params};
    const int top = 5853300;

    // The deviation is symmetric: an order priced far below the top is just as
    // much a fat finger as one priced far above it.
    EXPECT_FALSE(risk.runAllChecks(top - params.max_price_book_top_deviation - 1, 10, top));
}

TEST(RiskManager, RealisticPriceDoesNotOverflow) {
    RiskManager risk{defaults()};
    // LOBSTER prices are dollars x 10,000, so a live AAPL price is ~5.8e6.
    // Comparing against top * threshold would overflow int here.
    const int top = 5853300;
    EXPECT_TRUE(risk.runAllChecks(top + 500, 10, top));
    EXPECT_FALSE(risk.runAllChecks(top * 2, 10, top));
}

TEST(RiskManager, RunAllChecksRejectsWhenAnyCheckRejects) {
    RiskParams params = defaults();
    RiskManager risk{params};
    const int top = 5853300;

    // Price fine, quantity bad.
    EXPECT_FALSE(risk.runAllChecks(top, 0, top));
    // Quantity fine, price bad.
    EXPECT_FALSE(risk.runAllChecks(top * 2, 10, top));
    // Both bad.
    EXPECT_FALSE(risk.runAllChecks(top * 2, 0, top));
    // Both fine.
    EXPECT_TRUE(risk.runAllChecks(top, 10, top));
}

TEST(RiskManager, HonoursCustomParams) {
    RiskParams params;
    params.min_allowed_quantity_quote = 100;
    params.max_allowed_quantity_quote = 200;
    params.max_price_book_top_deviation = 5;
    RiskManager risk{params};

    EXPECT_FALSE(risk.runAllChecks(100, 99, std::nullopt));
    EXPECT_TRUE(risk.runAllChecks(100, 100, std::nullopt));
    EXPECT_TRUE(risk.runAllChecks(100, 200, std::nullopt));
    EXPECT_FALSE(risk.runAllChecks(100, 201, std::nullopt));

    EXPECT_TRUE(risk.runAllChecks(105, 100, 100));
    EXPECT_FALSE(risk.runAllChecks(106, 100, 100));
}
