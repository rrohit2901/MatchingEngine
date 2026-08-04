#include "OrderBookSide.h"

#include <gtest/gtest.h>

TEST(OrderBookSide, ConstructorBuy) {
    OrderBookSide<OrderSide::BUY> orderBookSideBuy{};
    EXPECT_EQ(orderBookSideBuy.getSide(), OrderSide::BUY);
    EXPECT_TRUE(orderBookSideBuy.getLevels().empty());
}

TEST (OrderBookSide, ConstructorSell) {
    OrderBookSide<OrderSide::SELL> orderBookSideSell{};
    EXPECT_EQ(orderBookSideSell.getSide(), OrderSide::SELL);
    EXPECT_TRUE(orderBookSideSell.getLevels().empty());
}

TEST (OrderBookSide, AddOrder) {
    OrderBookSide<OrderSide::BUY> orderBookSideBuy{};
    auto order = orderBookSideBuy.addOrder(1, OrderType::LIMIT, 1, 100);
    EXPECT_TRUE(orderBookSideBuy.isOrderIdExist(1));
    EXPECT_EQ(orderBookSideBuy.getLevels().size(), 1);
    EXPECT_EQ(order->getOrderId(), 1);
    EXPECT_EQ(order->getSide(), OrderSide::BUY);
    EXPECT_EQ(order->getType(), OrderType::LIMIT);
    EXPECT_EQ(order->getPrice(), 1);
    EXPECT_EQ(order->getQuantity(), 100);

    auto order2 = orderBookSideBuy.addOrder(2, OrderType::LIMIT, 2, 100);
    EXPECT_TRUE(orderBookSideBuy.isOrderIdExist(2));
    EXPECT_EQ(orderBookSideBuy.getLevels().size(), 2);
    EXPECT_EQ(order2->getOrderId(), 2);
    EXPECT_EQ(order2->getSide(), OrderSide::BUY);
    EXPECT_EQ(order2->getType(), OrderType::LIMIT);
    EXPECT_EQ(order2->getPrice(), 2);
    EXPECT_EQ(order2->getQuantity(), 100);
}

TEST (OrderBookSide, AddMarketOrder) {
    OrderBookSide<OrderSide::BUY> orderBookSideBuy{};
    auto order = orderBookSideBuy.addOrder(1, OrderType::MARKET, 0, 100);
    EXPECT_TRUE(orderBookSideBuy.isOrderIdExist(1));
    EXPECT_EQ(orderBookSideBuy.getLevels().size(), 1);
    EXPECT_EQ(order->getOrderId(), 1);
    EXPECT_EQ(order->getSide(), OrderSide::BUY);
    EXPECT_EQ(order->getType(), OrderType::MARKET);
    EXPECT_EQ(order->getPrice(), std::numeric_limits<int>::max());
    EXPECT_EQ(order->getQuantity(), 100);

    OrderBookSide<OrderSide::SELL> orderBookSideSell{};
    auto order2 = orderBookSideSell.addOrder(2, OrderType::MARKET, 0, 100);
    EXPECT_TRUE(orderBookSideSell.isOrderIdExist(2));
    EXPECT_EQ(orderBookSideSell.getLevels().size(), 1);
    EXPECT_EQ(order2->getOrderId(), 2);
    EXPECT_EQ(order2->getSide(), OrderSide::SELL);
    EXPECT_EQ(order2->getType(), OrderType::MARKET);
    EXPECT_EQ(order2->getPrice(), std::numeric_limits<int>::min());
    EXPECT_EQ(order2->getQuantity(), 100);
}


TEST (OrderBookSide, CancelOrder) {
    OrderBookSide<OrderSide::BUY> orderBookSideBuy{};
    auto order = orderBookSideBuy.addOrder(1, OrderType::LIMIT, 1, 100);
    EXPECT_TRUE(orderBookSideBuy.cancelOrder(1));
    EXPECT_FALSE(orderBookSideBuy.isOrderIdExist(1));
    EXPECT_TRUE(orderBookSideBuy.getLevels().empty());
}

TEST (OrderBookSide, ModifyOrder) {
    OrderBookSide<OrderSide::BUY> orderBookSideBuy{};
    auto order = orderBookSideBuy.addOrder(1, OrderType::LIMIT, 1, 100);
    auto modifiedOrder = orderBookSideBuy.modifyOrder(1, 50, 2);
    EXPECT_TRUE(orderBookSideBuy.isOrderIdExist(1));
    EXPECT_EQ(orderBookSideBuy.getLevels().size(), 1);
    EXPECT_EQ(modifiedOrder->getOrderId(), 1);
    EXPECT_EQ(modifiedOrder->getSide(), OrderSide::BUY);
    EXPECT_EQ(modifiedOrder->getType(), OrderType::LIMIT);
    EXPECT_EQ(modifiedOrder->getPrice(), 2);
    EXPECT_EQ(modifiedOrder->getQuantity(), 50);
}
