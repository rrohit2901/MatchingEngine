#include "gtest/gtest.h"
#include "Order.h"
#include "OrderBook.h"
#include "Matcher.h"

TEST (Matcher, FullMatchEqualQuantity) {
    OrderBook order_book;
    int sell_id = order_book.addOrder(100.0, 10, OrderType::LIMIT, OrderSide::SELL);
    int buy_id = order_book.addOrder(100.0, 10, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);
    EXPECT_TRUE(matcher.tryMatch(buy_id)); // incoming buy fully filled
    EXPECT_FALSE(order_book.getOrder(buy_id)->valid());
    EXPECT_FALSE(order_book.getOrder(sell_id)->valid());
}

TEST (Matcher, PartialMatchIncomingLarger) {
    OrderBook order_book;
    int sell_id = order_book.addOrder(100.0, 5, OrderType::LIMIT, OrderSide::SELL);
    int buy_id = order_book.addOrder(100.0, 10, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);
    EXPECT_FALSE(matcher.tryMatch(buy_id)); // 5 remaining, not fully filled
    EXPECT_TRUE(order_book.getOrder(buy_id)->valid());
    EXPECT_EQ(order_book.getOrder(buy_id)->getQuantity(), 5); // leftover rests
    EXPECT_FALSE(order_book.getOrder(sell_id)->valid()); // resting sell consumed
}

TEST (Matcher, PartialMatchRestingLarger) {
    OrderBook order_book;
    int sell_id = order_book.addOrder(100.0, 10, OrderType::LIMIT, OrderSide::SELL);
    int buy_id = order_book.addOrder(100.0, 5, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);
    EXPECT_TRUE(matcher.tryMatch(buy_id)); // incoming buy fully filled
    EXPECT_FALSE(order_book.getOrder(buy_id)->valid());
    EXPECT_TRUE(order_book.getOrder(sell_id)->valid()); // resting sell partially filled
    EXPECT_EQ(order_book.getOrder(sell_id)->getQuantity(), 5);
}

TEST (Matcher, NoMatchWhenPriceDoesNotCross) {
    OrderBook order_book;
    int sell_id = order_book.addOrder(101.0, 10, OrderType::LIMIT, OrderSide::SELL);
    int buy_id = order_book.addOrder(100.0, 10, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);
    EXPECT_FALSE(matcher.tryMatch(buy_id)); // buy below best ask, nothing crosses
    EXPECT_TRUE(order_book.getOrder(buy_id)->valid());
    EXPECT_EQ(order_book.getOrder(buy_id)->getQuantity(), 10);
    EXPECT_TRUE(order_book.getOrder(sell_id)->valid());
    EXPECT_EQ(order_book.getOrder(sell_id)->getQuantity(), 10);
}

TEST (Matcher, IncomingSellCrossesBuyBook) {
    OrderBook order_book;
    int buy_id = order_book.addOrder(100.0, 10, OrderType::LIMIT, OrderSide::BUY);
    int sell_id = order_book.addOrder(100.0, 10, OrderType::LIMIT, OrderSide::SELL);
    Matcher matcher(order_book);
    EXPECT_TRUE(matcher.tryMatch(sell_id)); // incoming sell fully filled against buy book
    EXPECT_FALSE(order_book.getOrder(sell_id)->valid());
    EXPECT_FALSE(order_book.getOrder(buy_id)->valid());
}

TEST (Matcher, MatchesBestPriceFirst) {
    OrderBook order_book;
    int best_ask = order_book.addOrder(100.0, 5, OrderType::LIMIT, OrderSide::SELL);
    int worse_ask = order_book.addOrder(100.5, 5, OrderType::LIMIT, OrderSide::SELL);
    int buy_id = order_book.addOrder(100.5, 5, OrderType::LIMIT, OrderSide::BUY);
    Matcher matcher(order_book);
    EXPECT_TRUE(matcher.tryMatch(buy_id)); // 5 units fill against the best ask only
    EXPECT_FALSE(order_book.getOrder(buy_id)->valid());
    EXPECT_FALSE(order_book.getOrder(best_ask)->valid()); // best (lowest) ask consumed
    EXPECT_TRUE(order_book.getOrder(worse_ask)->valid()); // worse ask untouched
    EXPECT_EQ(order_book.getOrder(worse_ask)->getQuantity(), 5);
}
