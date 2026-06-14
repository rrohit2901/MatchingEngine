#include "Order.h"

#include <gtest/gtest.h>

TEST(OrderTest, TestOrderCreation) {
    Order order(1, OrderSide::BUY, OrderType::LIMIT, 10000, 10);
    EXPECT_EQ(order.getOrderId(), 1);
    EXPECT_EQ(order.getSide(), OrderSide::BUY);
    EXPECT_EQ(order.getType(), OrderType::LIMIT);
    EXPECT_EQ(order.getPrice(), 10000);
    EXPECT_EQ(order.getQuantity(), 10);
    EXPECT_TRUE(order.valid());
}

TEST(OrderTest, TestOrderModification) {
    Order order(1, OrderSide::BUY, OrderType::LIMIT, 10000, 10);
    EXPECT_TRUE(order.modify(5, 9000));
    EXPECT_EQ(order.getQuantity(), 5);
    EXPECT_EQ(order.getPrice(), 9000);
}

TEST(OrderTest, TestOrderCancellation) {
    Order order(1, OrderSide::BUY, OrderType::LIMIT, 10000, 10);
    EXPECT_TRUE(order.cancel());
    EXPECT_FALSE(order.valid());
}

TEST(OrderTest, TestInvalidModification) {
    Order order(1, OrderSide::BUY, OrderType::LIMIT, 10000, 10);
    order.cancel();

    EXPECT_FALSE(order.modify(15, 11000)); // Invalid modification
    EXPECT_EQ(order.getQuantity(), 10); // Quantity should remain unchanged
    EXPECT_EQ(order.getPrice(), 10000); // Price should remain unchanged
}

TEST(OrderTest, TestInvalidCancellation) {
    Order order(1, OrderSide::BUY, OrderType::LIMIT, 10000, 10);
    order.cancel();

    EXPECT_FALSE(order.cancel()); // Invalid cancellation
    EXPECT_FALSE(order.valid()); // Order should still be invalid
}
