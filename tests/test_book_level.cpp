#include "BookLevel.h"

#include <gtest/gtest.h>

TEST (BookLevel, Constructor) {
    BookLevel bookLevel(1);
    EXPECT_EQ(bookLevel.getPrice(), 0.0001);
    EXPECT_EQ(bookLevel.getTotalQuantity(), 0);
    EXPECT_TRUE(bookLevel.getOrders().empty());
}

TEST (BookLevel, AddOrder) {
    BookLevel bookLevel(1);
    auto order = bookLevel.addOrder(1, OrderSide::BUY, OrderType::LIMIT, 1, 100);
    EXPECT_EQ(bookLevel.getTotalQuantity(), 100);
    EXPECT_EQ(bookLevel.getOrders().size(), 1);
    EXPECT_EQ(order->getOrderId(), 1);
    EXPECT_EQ(order->getSide(), OrderSide::BUY);
    EXPECT_EQ(order->getType(), OrderType::LIMIT);
    EXPECT_EQ(order->getPrice(), 1);
    EXPECT_EQ(order->getQuantity(), 100);
}

TEST (BookLevel, ModifyOrderLessQty) {
    BookLevel bookLevel(2);
    auto order = bookLevel.addOrder(1, OrderSide::BUY, OrderType::LIMIT, 2, 100);
    auto modifiedOrder = bookLevel.modifyOrder(order, 50, 2);
    EXPECT_EQ(bookLevel.getTotalQuantity(), 50);
    EXPECT_EQ(modifiedOrder->getPrice(), 2);
    EXPECT_EQ(modifiedOrder->getQuantity(), 50);
}

TEST (BookLevel, ModifyOrderMoreQty) {
    BookLevel bookLevel(2);
    auto order = bookLevel.addOrder(1, OrderSide::BUY, OrderType::LIMIT, 2, 100);
    auto modifiedOrder = bookLevel.modifyOrder(order, 150, 2);
    EXPECT_EQ(bookLevel.getTotalQuantity(), 150);
    EXPECT_EQ(modifiedOrder->getPrice(), 2);
    EXPECT_EQ(modifiedOrder->getQuantity(), 150);
    EXPECT_NE(modifiedOrder, order);
}

TEST (BookLevel, ModifyOrderLessQty_2_Orders) {
    BookLevel bookLevel(2);
    auto order1 = bookLevel.addOrder(1, OrderSide::BUY, OrderType::LIMIT, 2, 100);
    auto order2 = bookLevel.addOrder(2, OrderSide::BUY, OrderType::LIMIT, 2, 50);
    auto modifiedOrder = bookLevel.modifyOrder(order1, 50, 2);
    EXPECT_EQ(bookLevel.getTotalQuantity(), 100);
    EXPECT_EQ(modifiedOrder->getPrice(), 2);
    EXPECT_EQ(modifiedOrder->getQuantity(), 50);
    EXPECT_EQ(bookLevel.getOrders().size(), 2);
}

TEST (BookLevel, ModifyOrderMoreQty_2_Orders) {
    BookLevel bookLevel(2);
    auto order1 = bookLevel.addOrder(1, OrderSide::BUY, OrderType::LIMIT, 2, 100);
    auto order2 = bookLevel.addOrder(2, OrderSide::BUY, OrderType::LIMIT, 2, 50);
    auto modifiedOrder = bookLevel.modifyOrder(order1, 150, 2);
    EXPECT_EQ(bookLevel.getTotalQuantity(), 200);
    EXPECT_EQ(modifiedOrder->getPrice(), 2);
    EXPECT_EQ(modifiedOrder->getQuantity(), 150);
    EXPECT_EQ(bookLevel.getOrders().size(), 3);
    EXPECT_FALSE(order1->valid());
    EXPECT_NE(modifiedOrder, order1);
}

TEST (BookLevel, CancelOrder) {
    BookLevel bookLevel(2);
    auto order = bookLevel.addOrder(1, OrderSide::BUY, OrderType::LIMIT, 2, 100);
    bool cancelResult = bookLevel.cancelOrder(order);
    EXPECT_TRUE(cancelResult);
    EXPECT_EQ(bookLevel.getTotalQuantity(), 0);
    EXPECT_FALSE(order->valid());
}

