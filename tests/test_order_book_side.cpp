#include "OrderBookSide.h"
#include "OrderManager.h"

#include <gtest/gtest.h>

#include <memory>

// OrderBookSide is no longer default-constructible: it shares an OrderManager
// with the rest of the book and is constructed from it. addOrder() no longer
// takes a caller-supplied order id — the OrderManager mints ids — and returns
// the new id rather than an Order object.

namespace {

class OrderBookSideTest : public ::testing::Test {
  protected:
    std::shared_ptr<OrderManager> order_manager{std::make_shared<OrderManager>()};
};

} // namespace

TEST_F(OrderBookSideTest, ConstructorBuy) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};
    EXPECT_EQ(buy.getSide(), OrderSide::BUY);
    EXPECT_TRUE(buy.getLevels().empty());
}

TEST_F(OrderBookSideTest, ConstructorSell) {
    OrderBookSide<OrderSide::SELL> sell{order_manager};
    EXPECT_EQ(sell.getSide(), OrderSide::SELL);
    EXPECT_TRUE(sell.getLevels().empty());
}

TEST_F(OrderBookSideTest, AddOrder) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};

    const order_id_t id1 = buy.addOrder(OrderType::LIMIT, 100, 10);
    EXPECT_TRUE(buy.isOrderIdExist(id1));
    EXPECT_EQ(buy.getLevels().size(), 1u);
    EXPECT_EQ(order_manager->getSide(id1).value(), OrderSide::BUY);
    EXPECT_EQ(order_manager->getType(id1).value(), OrderType::LIMIT);
    EXPECT_EQ(order_manager->getPrice(id1).value(), 100);
    EXPECT_EQ(order_manager->getQuantity(id1).value(), 10);

    // A second price opens a second level.
    const order_id_t id2 = buy.addOrder(OrderType::LIMIT, 101, 10);
    EXPECT_TRUE(buy.isOrderIdExist(id2));
    EXPECT_EQ(buy.getLevels().size(), 2u);
    EXPECT_EQ(order_manager->getPrice(id2).value(), 101);
}

TEST_F(OrderBookSideTest, AddTwoOrdersAtSamePriceShareOneLevel) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};
    buy.addOrder(OrderType::LIMIT, 100, 10);
    buy.addOrder(OrderType::LIMIT, 100, 5);

    const auto levels = buy.getLevels();
    ASSERT_EQ(levels.size(), 1u);
    EXPECT_EQ(levels.front()->getPrice(), 100);
    EXPECT_EQ(levels.front()->getTotalQuantity(), 15);
}

TEST_F(OrderBookSideTest, IsOrderIdExistRejectsOtherSide) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};
    OrderBookSide<OrderSide::SELL> sell{order_manager};

    const order_id_t buy_id = buy.addOrder(OrderType::LIMIT, 100, 10);
    EXPECT_TRUE(buy.isOrderIdExist(buy_id));
    EXPECT_FALSE(sell.isOrderIdExist(buy_id));
    EXPECT_FALSE(buy.isOrderIdExist(9999));
}

TEST_F(OrderBookSideTest, CancelOrderDropsEmptiedLevel) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};
    const order_id_t id = buy.addOrder(OrderType::LIMIT, 100, 10);

    EXPECT_TRUE(buy.cancelOrder(id));
    EXPECT_FALSE(buy.isOrderIdExist(id));
    EXPECT_TRUE(buy.getLevels().empty());
}

TEST_F(OrderBookSideTest, CancelOrderKeepsLevelWithRemainingQuantity) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};
    const order_id_t id1 = buy.addOrder(OrderType::LIMIT, 100, 10);
    const order_id_t id2 = buy.addOrder(OrderType::LIMIT, 100, 5);

    EXPECT_TRUE(buy.cancelOrder(id1));
    EXPECT_TRUE(buy.isOrderIdExist(id2));

    const auto levels = buy.getLevels();
    ASSERT_EQ(levels.size(), 1u);
    EXPECT_EQ(levels.front()->getTotalQuantity(), 5);
}

TEST_F(OrderBookSideTest, CancelUnknownOrderFails) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};
    EXPECT_FALSE(buy.cancelOrder(9999));
}

TEST_F(OrderBookSideTest, ModifyOrderSamePrice) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};
    const order_id_t id = buy.addOrder(OrderType::LIMIT, 100, 100);

    const order_id_t modified = buy.modifyOrder(id, 50, 100);
    ASSERT_NE(modified, 0u); // 0 is the not-found sentinel
    EXPECT_TRUE(buy.isOrderIdExist(modified));
    EXPECT_EQ(buy.getLevels().size(), 1u);
    EXPECT_EQ(order_manager->getPrice(modified).value(), 100);
    EXPECT_EQ(order_manager->getQuantity(modified).value(), 50);
}

TEST_F(OrderBookSideTest, ModifyOrderNewPriceMovesLevel) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};
    const order_id_t id = buy.addOrder(OrderType::LIMIT, 100, 100);

    const order_id_t modified = buy.modifyOrder(id, 50, 101);
    ASSERT_NE(modified, 0u);
    EXPECT_FALSE(buy.isOrderIdExist(id)); // repriced orders lose their id
    EXPECT_TRUE(buy.isOrderIdExist(modified));

    const auto levels = buy.getLevels();
    ASSERT_EQ(levels.size(), 1u); // old level emptied and dropped
    EXPECT_EQ(levels.front()->getPrice(), 101);
    EXPECT_EQ(order_manager->getQuantity(modified).value(), 50);
}

TEST_F(OrderBookSideTest, ModifyUnknownOrderReturnsZero) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};
    EXPECT_EQ(buy.modifyOrder(9999, 10, 100), 0u);
}

TEST_F(OrderBookSideTest, BuyLevelsSortHighestFirst) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};
    buy.addOrder(OrderType::LIMIT, 100, 10);
    buy.addOrder(OrderType::LIMIT, 102, 10);
    buy.addOrder(OrderType::LIMIT, 101, 10);

    const auto view = buy.getBookSideView(3);
    ASSERT_EQ(view.size(), 3u);
    EXPECT_EQ(view[0]->getPrice(), 102); // best bid first
    EXPECT_EQ(view[1]->getPrice(), 101);
    EXPECT_EQ(view[2]->getPrice(), 100);
}

TEST_F(OrderBookSideTest, SellLevelsSortLowestFirst) {
    OrderBookSide<OrderSide::SELL> sell{order_manager};
    sell.addOrder(OrderType::LIMIT, 102, 10);
    sell.addOrder(OrderType::LIMIT, 100, 10);
    sell.addOrder(OrderType::LIMIT, 101, 10);

    const auto view = sell.getBookSideView(3);
    ASSERT_EQ(view.size(), 3u);
    EXPECT_EQ(view[0]->getPrice(), 100); // best ask first
    EXPECT_EQ(view[1]->getPrice(), 101);
    EXPECT_EQ(view[2]->getPrice(), 102);
}

TEST_F(OrderBookSideTest, BookSideViewRespectsLevelCap) {
    OrderBookSide<OrderSide::BUY> buy{order_manager};
    buy.addOrder(OrderType::LIMIT, 100, 10);
    buy.addOrder(OrderType::LIMIT, 101, 10);
    buy.addOrder(OrderType::LIMIT, 102, 10);

    EXPECT_EQ(buy.getBookSideView(2).size(), 2u);
    EXPECT_EQ(buy.getBookSideView(1).size(), 1u);
    EXPECT_EQ(buy.getBookSideView(10).size(), 3u); // cap above depth returns all
}

TEST_F(OrderBookSideTest, FillOrdersConsumesBestPriceFirst) {
    OrderBookSide<OrderSide::SELL> sell{order_manager};
    const order_id_t best = sell.addOrder(OrderType::LIMIT, 100, 5);
    const order_id_t worse = sell.addOrder(OrderType::LIMIT, 101, 5);

    // A buyer willing to pay 100 can only reach the 100 level.
    EXPECT_EQ(sell.fillOrders(100, 5), 0);
    EXPECT_FALSE(order_manager->valid(best));
    ASSERT_TRUE(order_manager->valid(worse));
    EXPECT_EQ(order_manager->getQuantity(worse).value(), 5);
}

TEST_F(OrderBookSideTest, FillOrdersReturnsUnfilledRemainder) {
    OrderBookSide<OrderSide::SELL> sell{order_manager};
    sell.addOrder(OrderType::LIMIT, 100, 5);

    // Only 5 units are reachable at 100, so 5 of the requested 10 go unfilled.
    EXPECT_EQ(sell.fillOrders(100, 10), 5);
}

TEST_F(OrderBookSideTest, FillOrdersSkipsLevelsThatDoNotCross) {
    OrderBookSide<OrderSide::SELL> sell{order_manager};
    const order_id_t ask = sell.addOrder(OrderType::LIMIT, 101, 5);

    // A buyer at 100 cannot reach a 101 ask; nothing fills.
    EXPECT_EQ(sell.fillOrders(100, 5), 5);
    ASSERT_TRUE(order_manager->valid(ask));
    EXPECT_EQ(order_manager->getQuantity(ask).value(), 5);
}
