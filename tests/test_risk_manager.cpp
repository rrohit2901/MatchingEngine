#include "RiskManager.h"

#include <gtest/gtest.h>

#include <optional>

// RiskManager is a pure predicate over (price, quantity, top-of-book): it holds
// no state and touches no book, so every case can be driven directly.
//
// checkOrder() must reject when ANY check rejects. The interesting cases are
// the boundaries, since an off-by-one there either lets a fat-finger order
// through or blocks legitimate flow.

namespace {

RiskParams defaults() {
    return RiskParams{};
}

} // namespace

TEST(RiskManager, AcceptsAnOrdinaryOrder) {
    RiskManager risk{defaults()};
    EXPECT_EQ(risk.checkOrder(100, 10, std::nullopt), RejectReason::NONE);
    EXPECT_EQ(risk.checkOrder(100, 10, 100), RejectReason::NONE);
}

TEST(RiskManager, RejectsQuantityAboveMax) {
    RiskParams params = defaults();
    RiskManager risk{params};

    EXPECT_EQ(risk.checkOrder(100, params.max_allowed_quantity_quote, std::nullopt), RejectReason::NONE);
    EXPECT_NE(risk.checkOrder(100, params.max_allowed_quantity_quote + 1, std::nullopt), RejectReason::NONE);
}

TEST(RiskManager, RejectsQuantityBelowMin) {
    RiskParams params = defaults();
    RiskManager risk{params};

    EXPECT_EQ(risk.checkOrder(100, params.min_allowed_quantity_quote, std::nullopt), RejectReason::NONE);
    EXPECT_NE(risk.checkOrder(100, params.min_allowed_quantity_quote - 1, std::nullopt), RejectReason::NONE);
}

TEST(RiskManager, RejectsZeroAndNegativeQuantity) {
    RiskManager risk{defaults()};
    EXPECT_NE(risk.checkOrder(100, 0, std::nullopt), RejectReason::NONE);
    EXPECT_NE(risk.checkOrder(100, -5, std::nullopt), RejectReason::NONE);
}

TEST(RiskManager, NoPriceCheckWhenBookIsEmpty) {
    RiskManager risk{defaults()};
    // With no top of book there is nothing to deviate from, so any price passes.
    EXPECT_EQ(risk.checkOrder(1, 10, std::nullopt), RejectReason::NONE);
    EXPECT_EQ(risk.checkOrder(99999999, 10, std::nullopt), RejectReason::NONE);
}

TEST(RiskManager, AcceptsPriceWithinDeviationOfTop) {
    RiskParams params = defaults();
    RiskManager risk{params};
    const int top = 5853300;

    EXPECT_EQ(risk.checkOrder(top, 10, top), RejectReason::NONE);
    EXPECT_EQ(risk.checkOrder(top + params.max_price_book_top_deviation, 10, top), RejectReason::NONE);
    EXPECT_EQ(risk.checkOrder(top - params.max_price_book_top_deviation, 10, top), RejectReason::NONE);
}

TEST(RiskManager, RejectsPriceTooFarAboveTop) {
    RiskParams params = defaults();
    RiskManager risk{params};
    const int top = 5853300;

    EXPECT_NE(risk.checkOrder(top + params.max_price_book_top_deviation + 1, 10, top), RejectReason::NONE);
}

TEST(RiskManager, RejectsPriceTooFarBelowTop) {
    RiskParams params = defaults();
    RiskManager risk{params};
    const int top = 5853300;

    // The deviation is symmetric: an order priced far below the top is just as
    // much a fat finger as one priced far above it.
    EXPECT_NE(risk.checkOrder(top - params.max_price_book_top_deviation - 1, 10, top), RejectReason::NONE);
}

TEST(RiskManager, RealisticPriceDoesNotOverflow) {
    RiskManager risk{defaults()};
    // Replay prices are ticks of 1e-4 dollars, so a live AAPL price is ~5.8e6.
    // Comparing against top * threshold would overflow int here.
    const int top = 5853300;
    EXPECT_EQ(risk.checkOrder(top + 500, 10, top), RejectReason::NONE);
    EXPECT_NE(risk.checkOrder(top * 2, 10, top), RejectReason::NONE);
}

TEST(RiskManager, CheckOrderRejectsWhenAnyCheckRejects) {
    RiskParams params = defaults();
    RiskManager risk{params};
    const int top = 5853300;

    // Price fine, quantity bad.
    EXPECT_NE(risk.checkOrder(top, 0, top), RejectReason::NONE);
    // Quantity fine, price bad.
    EXPECT_NE(risk.checkOrder(top * 2, 10, top), RejectReason::NONE);
    // Both bad.
    EXPECT_NE(risk.checkOrder(top * 2, 0, top), RejectReason::NONE);
    // Both fine.
    EXPECT_EQ(risk.checkOrder(top, 10, top), RejectReason::NONE);
}

TEST(RiskManager, HonoursCustomParams) {
    RiskParams params;
    params.min_allowed_quantity_quote = 100;
    params.max_allowed_quantity_quote = 200;
    params.max_price_book_top_deviation = 5;
    RiskManager risk{params};

    EXPECT_NE(risk.checkOrder(100, 99, std::nullopt), RejectReason::NONE);
    EXPECT_EQ(risk.checkOrder(100, 100, std::nullopt), RejectReason::NONE);
    EXPECT_EQ(risk.checkOrder(100, 200, std::nullopt), RejectReason::NONE);
    EXPECT_NE(risk.checkOrder(100, 201, std::nullopt), RejectReason::NONE);

    EXPECT_EQ(risk.checkOrder(105, 100, 100), RejectReason::NONE);
    EXPECT_NE(risk.checkOrder(106, 100, 100), RejectReason::NONE);
}

// --- reject reasons --------------------------------------------------------
// checkOrder() reports WHY, so a reject log can be acted on.

TEST(RiskManager, CheckOrderReportsNoneWhenAccepted) {
    RiskManager risk{defaults()};
    EXPECT_EQ(risk.checkOrder(100, 10, std::nullopt), RejectReason::NONE);
    EXPECT_EQ(risk.checkOrder(100, 10, 100), RejectReason::NONE);
}

TEST(RiskManager, CheckOrderDistinguishesQuantityBounds) {
    RiskParams params = defaults();
    RiskManager risk{params};

    EXPECT_EQ(risk.checkOrder(100, params.max_allowed_quantity_quote + 1, std::nullopt),
              RejectReason::QUANTITY_ABOVE_MAX);
    EXPECT_EQ(risk.checkOrder(100, 0, std::nullopt), RejectReason::QUANTITY_BELOW_MIN);
    EXPECT_EQ(risk.checkOrder(100, -5, std::nullopt), RejectReason::QUANTITY_BELOW_MIN);
}

TEST(RiskManager, CheckOrderReportsPriceDeviation) {
    RiskParams params = defaults();
    RiskManager risk{params};
    const int top = 5853300;

    EXPECT_EQ(risk.checkOrder(top + params.max_price_book_top_deviation + 1, 10, top),
              RejectReason::PRICE_TOO_FAR_FROM_TOP);
    EXPECT_EQ(risk.checkOrder(top - params.max_price_book_top_deviation - 1, 10, top),
              RejectReason::PRICE_TOO_FAR_FROM_TOP);
}

TEST(RiskManager, QuantityIsReportedBeforePrice) {
    RiskManager risk{defaults()};
    const int top = 5853300;
    // Both checks fail; the reason reported is the first one evaluated, so the
    // log stays deterministic rather than depending on check ordering by luck.
    EXPECT_EQ(risk.checkOrder(top * 2, 0, top), RejectReason::QUANTITY_BELOW_MIN);
}

TEST(RiskManager, ReasonsHaveDistinctNames) {
    EXPECT_STREQ(to_string(RejectReason::NONE), "NONE");
    EXPECT_STREQ(to_string(RejectReason::QUANTITY_ABOVE_MAX), "QUANTITY_ABOVE_MAX");
    EXPECT_STREQ(to_string(RejectReason::QUANTITY_BELOW_MIN), "QUANTITY_BELOW_MIN");
    EXPECT_STREQ(to_string(RejectReason::PRICE_TOO_FAR_FROM_TOP), "PRICE_TOO_FAR_FROM_TOP");
}
