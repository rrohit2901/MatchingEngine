#include "gtest/gtest.h"
#include "Order.h"
#include "OrderBook.h"
#include "Matcher.h"

#include <memory>

// Matcher holds the book by shared_ptr, so tests build the book on the heap and
// hand the same instance to both the test body and the matcher.
//
// OrderBook::getOrder() is gone; order state is read through the optional
// accessors, and an order that has left the book reports IsOrderValid() == false.
//
// Fill semantics under test:
//   - an order that is completely filled leaves the book;
//   - an order that is only partially filled keeps resting with the quantity
//     that did NOT fill.

namespace {

::testing::AssertionResult IsGone(OrderBook& book, order_id_t order_id) {
    if (!book.IsOrderValid(order_id)) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "order " << order_id << " still in book with quantity "
           << book.getOrderQuantity(order_id).value_or(-1);
}

} // namespace

TEST(Matcher, FullMatchEqualQuantity) {
    auto order_book = std::make_shared<OrderBook>();
    const order_id_t sell_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t buy_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);

    EXPECT_TRUE(matcher.tryMatch(buy_id)); // incoming buy fully filled
    EXPECT_TRUE(IsGone(*order_book, buy_id));
    EXPECT_TRUE(IsGone(*order_book, sell_id));
}

TEST(Matcher, PartialMatchIncomingLarger) {
    auto order_book = std::make_shared<OrderBook>();
    const order_id_t sell_id = order_book->addOrder(100, 5, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t buy_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);

    EXPECT_FALSE(matcher.tryMatch(buy_id)); // 5 remaining, not fully filled
    EXPECT_TRUE(IsGone(*order_book, sell_id)); // resting sell consumed

    // The buy rests with its UNFILLED remainder, not the amount that traded.
    ASSERT_TRUE(order_book->IsOrderValid(buy_id))
        << "partially filled buy must keep resting in the book";
    EXPECT_EQ(order_book->getOrderQuantity(buy_id).value(), 5);
}

TEST(Matcher, PartialMatchRestingLarger) {
    auto order_book = std::make_shared<OrderBook>();
    const order_id_t sell_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t buy_id = order_book->addOrder(100, 5, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);

    EXPECT_TRUE(matcher.tryMatch(buy_id)); // incoming buy fully filled
    EXPECT_TRUE(IsGone(*order_book, buy_id));

    ASSERT_TRUE(order_book->IsOrderValid(sell_id))
        << "partially filled sell must keep resting in the book";
    EXPECT_EQ(order_book->getOrderQuantity(sell_id).value(), 5);
}

TEST(Matcher, NoMatchWhenPriceDoesNotCross) {
    auto order_book = std::make_shared<OrderBook>();
    const order_id_t sell_id = order_book->addOrder(101, 10, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t buy_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);

    EXPECT_FALSE(matcher.tryMatch(buy_id)); // buy below best ask, nothing crosses

    ASSERT_TRUE(order_book->IsOrderValid(buy_id))
        << "unmatched buy must keep resting in the book";
    EXPECT_EQ(order_book->getOrderQuantity(buy_id).value(), 10);

    ASSERT_TRUE(order_book->IsOrderValid(sell_id))
        << "untouched sell must keep resting in the book";
    EXPECT_EQ(order_book->getOrderQuantity(sell_id).value(), 10);
}

TEST(Matcher, IncomingSellCrossesBuyBook) {
    auto order_book = std::make_shared<OrderBook>();
    const order_id_t buy_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    const order_id_t sell_id = order_book->addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);
    Matcher matcher(order_book);

    EXPECT_TRUE(matcher.tryMatch(sell_id)); // incoming sell fully filled against buy book
    EXPECT_TRUE(IsGone(*order_book, sell_id));
    EXPECT_TRUE(IsGone(*order_book, buy_id));
}

TEST(Matcher, MatchesBestPriceFirst) {
    auto order_book = std::make_shared<OrderBook>();
    const order_id_t best_ask = order_book->addOrder(100, 5, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t worse_ask = order_book->addOrder(105, 5, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t buy_id = order_book->addOrder(105, 5, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);

    EXPECT_TRUE(matcher.tryMatch(buy_id)); // 5 units fill against the best ask only
    EXPECT_TRUE(IsGone(*order_book, buy_id));
    EXPECT_TRUE(IsGone(*order_book, best_ask)); // best (lowest) ask consumed

    ASSERT_TRUE(order_book->IsOrderValid(worse_ask)) << "worse ask must be untouched";
    EXPECT_EQ(order_book->getOrderQuantity(worse_ask).value(), 5);
}
