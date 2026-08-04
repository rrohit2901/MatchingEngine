#include "gtest/gtest.h"
#include "Order.h"
#include "OrderBook.h"
#include "Matcher.h"

#include <memory>

// Matcher now holds the book by shared_ptr, so tests build the book on the heap
// and hand the same instance to both the test body and the matcher.
//
// Fill semantics under test:
//   - an order that is completely filled is removed from the book, so
//     getOrder() returns nullptr for it;
//   - an order that is only partially filled keeps resting with its remaining
//     quantity and stays reachable via getOrder().

namespace {

// getOrder() returns a null shared_ptr once an order leaves the book, so every
// assertion that dereferences it needs this guard first.
::testing::AssertionResult IsGone(OrderBook& book, int order_id) {
    auto order = book.getOrder(order_id);
    if (order == nullptr) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "order " << order_id << " still in book with quantity "
           << order->getQuantity();
}

} // namespace

TEST (Matcher, FullMatchEqualQuantity) {
    auto order_book = std::make_shared<OrderBook>();
    int sell_id = order_book->addOrder(100.0, 10, OrderType::LIMIT, OrderSide::SELL);
    int buy_id = order_book->addOrder(100.0, 10, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);
    EXPECT_TRUE(matcher.tryMatch(buy_id)); // incoming buy fully filled
    EXPECT_TRUE(IsGone(*order_book, buy_id));
    EXPECT_TRUE(IsGone(*order_book, sell_id));
}

TEST (Matcher, PartialMatchIncomingLarger) {
    auto order_book = std::make_shared<OrderBook>();
    int sell_id = order_book->addOrder(100.0, 5, OrderType::LIMIT, OrderSide::SELL);
    int buy_id = order_book->addOrder(100.0, 10, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);
    EXPECT_FALSE(matcher.tryMatch(buy_id)); // 5 remaining, not fully filled
    EXPECT_TRUE(IsGone(*order_book, sell_id)); // resting sell consumed

    auto buy = order_book->getOrder(buy_id);
    ASSERT_NE(buy, nullptr) << "partially filled buy must keep resting in the book";
    EXPECT_TRUE(buy->valid());
    EXPECT_EQ(buy->getQuantity(), 5); // leftover rests
}

TEST (Matcher, PartialMatchRestingLarger) {
    auto order_book = std::make_shared<OrderBook>();
    int sell_id = order_book->addOrder(100.0, 10, OrderType::LIMIT, OrderSide::SELL);
    int buy_id = order_book->addOrder(100.0, 5, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);
    EXPECT_TRUE(matcher.tryMatch(buy_id)); // incoming buy fully filled
    EXPECT_TRUE(IsGone(*order_book, buy_id));

    auto sell = order_book->getOrder(sell_id);
    ASSERT_NE(sell, nullptr) << "partially filled sell must keep resting in the book";
    EXPECT_TRUE(sell->valid()); // resting sell partially filled
    EXPECT_EQ(sell->getQuantity(), 5);
}

TEST (Matcher, NoMatchWhenPriceDoesNotCross) {
    auto order_book = std::make_shared<OrderBook>();
    int sell_id = order_book->addOrder(101.0, 10, OrderType::LIMIT, OrderSide::SELL);
    int buy_id = order_book->addOrder(100.0, 10, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);
    EXPECT_FALSE(matcher.tryMatch(buy_id)); // buy below best ask, nothing crosses

    auto buy = order_book->getOrder(buy_id);
    ASSERT_NE(buy, nullptr) << "unmatched buy must keep resting in the book";
    EXPECT_TRUE(buy->valid());
    EXPECT_EQ(buy->getQuantity(), 10);

    auto sell = order_book->getOrder(sell_id);
    ASSERT_NE(sell, nullptr) << "untouched sell must keep resting in the book";
    EXPECT_TRUE(sell->valid());
    EXPECT_EQ(sell->getQuantity(), 10);
}

TEST (Matcher, IncomingSellCrossesBuyBook) {
    auto order_book = std::make_shared<OrderBook>();
    int buy_id = order_book->addOrder(100.0, 10, OrderType::LIMIT, OrderSide::BUY);
    int sell_id = order_book->addOrder(100.0, 10, OrderType::LIMIT, OrderSide::SELL);
    Matcher matcher(order_book);
    EXPECT_TRUE(matcher.tryMatch(sell_id)); // incoming sell fully filled against buy book
    EXPECT_TRUE(IsGone(*order_book, sell_id));
    EXPECT_TRUE(IsGone(*order_book, buy_id));
}

TEST (Matcher, MatchesBestPriceFirst) {
    auto order_book = std::make_shared<OrderBook>();
    int best_ask = order_book->addOrder(100.0, 5, OrderType::LIMIT, OrderSide::SELL);
    int worse_ask = order_book->addOrder(100.5, 5, OrderType::LIMIT, OrderSide::SELL);
    int buy_id = order_book->addOrder(100.5, 5, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);
    EXPECT_TRUE(matcher.tryMatch(buy_id)); // 5 units fill against the best ask only
    EXPECT_TRUE(IsGone(*order_book, buy_id));
    EXPECT_TRUE(IsGone(*order_book, best_ask)); // best (lowest) ask consumed

    auto worse = order_book->getOrder(worse_ask);
    ASSERT_NE(worse, nullptr) << "worse ask must be untouched";
    EXPECT_TRUE(worse->valid());
    EXPECT_EQ(worse->getQuantity(), 5);
}
