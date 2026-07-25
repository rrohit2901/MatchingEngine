#include "gtest/gtest.h"
#include "Order.h"
#include "OrderBook.h"

TEST (OrderBook, AddOrder) {
    OrderBook order_book;
    int order_id = order_book.addOrder(100.0, 10, OrderType::LIMIT, OrderSide::BUY);
    EXPECT_EQ(order_book.getOrder(order_id)->getOrderId(), order_id);
    EXPECT_EQ(order_book.getOrder(order_id)->getType(), OrderType::LIMIT);

    int order_id2 = order_book.addOrder(101.0, 5, OrderType::LIMIT, OrderSide::SELL);
    EXPECT_EQ(order_book.getOrder(order_id2)->getOrderId(), order_id2);
    EXPECT_EQ(order_book.getOrder(order_id2)->getType(), OrderType::LIMIT);

    int order_id3 = order_book.addOrder(99.0, 20, OrderType::MARKET, OrderSide::BUY);
    EXPECT_EQ(order_book.getOrder(order_id3)->getOrderId(), order_id3);
    EXPECT_EQ(order_book.getOrder(order_id3)->getType(), OrderType::MARKET);
}

TEST (OrderBook, CancelOrder) {
    OrderBook order_book;
    int order_id = order_book.addOrder(100.0, 10, OrderType::LIMIT, OrderSide::BUY);
    EXPECT_TRUE(order_book.cancelOrder(order_id));
    EXPECT_EQ(order_book.getOrder(order_id), nullptr);
}

TEST (OrderBook, ModifyOrder) {
    OrderBook order_book;
    int order_id = order_book.addOrder(100.0, 10, OrderType::LIMIT, OrderSide::BUY);
    EXPECT_TRUE(order_book.modifyOrder(order_id, 20, 101.0, OrderSide::SELL, OrderType::LIMIT));
    EXPECT_EQ(order_book.getOrder(order_id)->getQuantity(), 20);
    EXPECT_EQ(order_book.getOrder(order_id)->getPrice(), 1010000); // Price is stored as an integer multiplied by PRICE_MULTIPLIER
    EXPECT_EQ(order_book.getOrder(order_id)->getSide(), OrderSide::SELL);

    int order_id2 = order_book.addOrder(102.0, 5, OrderType::LIMIT, OrderSide::SELL);
    EXPECT_TRUE(order_book.modifyOrder(order_id2, 10, 103.0, OrderSide::SELL, OrderType::LIMIT));
    EXPECT_EQ(order_book.getOrder(order_id2)->getQuantity(), 10);
    EXPECT_EQ(order_book.getOrder(order_id2)->getPrice(), 1030000);
    EXPECT_EQ(order_book.getOrder(order_id2)->getSide(), OrderSide::SELL);
}
