#include "BookLevel.h"
#include "OrderManager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

// BookLevel no longer owns Order objects. It stores order ids and defers every
// per-order question to a shared OrderManager, so each test constructs a manager,
// hands it to the level, and reads order state back through it.

namespace {

// fillOrders() reports each fill as a TradeEvent and needs the id of the
// incoming order it is matching against. BookLevel never looks the id up, so
// tests can use a standalone one.
constexpr order_id_t kAggressor = 9999;

class BookLevelTest : public ::testing::Test {
  protected:
    std::shared_ptr<OrderManager> order_manager{std::make_shared<OrderManager>()};
};

} // namespace

TEST_F(BookLevelTest, Constructor) {
    BookLevel level(order_manager, 100);
    EXPECT_EQ(level.getPrice(), 100);
    EXPECT_EQ(level.getTotalQuantity(), 0);
    EXPECT_TRUE(level.getOrders().empty());
    EXPECT_TRUE(level.getAllOrders().empty());
}

TEST_F(BookLevelTest, AddOrder) {
    BookLevel level(order_manager, 100);
    const order_id_t id = level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 10);

    EXPECT_EQ(level.getTotalQuantity(), 10);
    ASSERT_EQ(level.getOrders().size(), 1u);
    EXPECT_EQ(level.getOrders().front(), id);

    ASSERT_TRUE(order_manager->valid(id));
    EXPECT_EQ(order_manager->getView(id).value().side, OrderSide::BUY);
    EXPECT_EQ(order_manager->getView(id).value().type, OrderType::LIMIT);
    EXPECT_EQ(order_manager->getView(id).value().price, 100);
    EXPECT_EQ(order_manager->getView(id).value().quantity, 10);
}

TEST_F(BookLevelTest, AddTwoOrdersAccumulatesQuantity) {
    BookLevel level(order_manager, 100);
    const order_id_t id1 = level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 10);
    const order_id_t id2 = level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 5);

    EXPECT_NE(id1, id2);
    EXPECT_EQ(level.getTotalQuantity(), 15);
    EXPECT_EQ(level.getOrders().size(), 2u);
}

TEST_F(BookLevelTest, ModifyOrderLessQtyKeepsSameId) {
    BookLevel level(order_manager, 100);
    const order_id_t id = level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 100);

    const auto modified = level.modifyOrder(id, 50, 100);
    ASSERT_TRUE(modified.has_value());
    // Shrinking in place keeps queue position, so the id survives.
    EXPECT_EQ(modified.value(), id);
    EXPECT_EQ(level.getTotalQuantity(), 50);
    EXPECT_EQ(order_manager->getView(id).value().quantity, 50);
    EXPECT_EQ(level.getOrders().size(), 1u);
}

TEST_F(BookLevelTest, ModifyOrderMoreQtyIssuesNewId) {
    BookLevel level(order_manager, 100);
    const order_id_t id = level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 100);

    const auto modified = level.modifyOrder(id, 150, 100);
    ASSERT_TRUE(modified.has_value());
    // Growing loses queue priority: the old order is cancelled and a new one added.
    EXPECT_NE(modified.value(), id);
    EXPECT_FALSE(order_manager->valid(id));
    EXPECT_EQ(level.getTotalQuantity(), 150);
    EXPECT_EQ(order_manager->getView(modified.value()).value().quantity, 150);

    ASSERT_EQ(level.getOrders().size(), 1u);
    EXPECT_EQ(level.getOrders().front(), modified.value());
}

TEST_F(BookLevelTest, ModifyOrderLessQtyWithTwoOrders) {
    BookLevel level(order_manager, 100);
    const order_id_t id1 = level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 100);
    level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 50);

    const auto modified = level.modifyOrder(id1, 50, 100);
    ASSERT_TRUE(modified.has_value());
    EXPECT_EQ(modified.value(), id1);
    EXPECT_EQ(level.getTotalQuantity(), 100);
    EXPECT_EQ(level.getOrders().size(), 2u);
}

TEST_F(BookLevelTest, ModifyOrderMoreQtyWithTwoOrders) {
    BookLevel level(order_manager, 100);
    const order_id_t id1 = level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 100);
    const order_id_t id2 = level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 50);

    const auto modified = level.modifyOrder(id1, 150, 100);
    ASSERT_TRUE(modified.has_value());
    EXPECT_NE(modified.value(), id1);
    EXPECT_EQ(level.getTotalQuantity(), 200);
    EXPECT_FALSE(order_manager->valid(id1));

    // getOrders() reports only live ids: id2 plus the replacement for id1.
    const auto live = level.getOrders();
    ASSERT_EQ(live.size(), 2u);
    EXPECT_NE(std::find(live.begin(), live.end(), id2), live.end());
    EXPECT_NE(std::find(live.begin(), live.end(), modified.value()), live.end());
}

TEST_F(BookLevelTest, ModifyUnknownOrderReturnsNullopt) {
    BookLevel level(order_manager, 100);
    EXPECT_FALSE(level.modifyOrder(42, 10, 100).has_value());
}

TEST_F(BookLevelTest, CancelOrder) {
    BookLevel level(order_manager, 100);
    const order_id_t id = level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 100);

    EXPECT_TRUE(level.cancelOrder(id));
    EXPECT_EQ(level.getTotalQuantity(), 0);
    EXPECT_FALSE(order_manager->valid(id));
    EXPECT_TRUE(level.getOrders().empty());
}

TEST_F(BookLevelTest, CancelOrderTwiceFails) {
    BookLevel level(order_manager, 100);
    const order_id_t id = level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 100);

    ASSERT_TRUE(level.cancelOrder(id));
    EXPECT_FALSE(level.cancelOrder(id));
    EXPECT_EQ(level.getTotalQuantity(), 0);
}

TEST_F(BookLevelTest, FillOrdersPartial) {
    BookLevel level(order_manager, 100);
    const order_id_t id1 = level.addOrder(OrderSide::SELL, OrderType::LIMIT, 100, 10);
    const order_id_t id2 = level.addOrder(OrderSide::SELL, OrderType::LIMIT, 100, 5);

    // 12 units consume id1 entirely and 2 of id2's 5.
    std::vector<TradeEvent> trades;
    EXPECT_EQ(level.fillOrders(12, trades, kAggressor), 0);
    EXPECT_EQ(level.getTotalQuantity(), 3);
    EXPECT_FALSE(order_manager->valid(id1));
    ASSERT_TRUE(order_manager->valid(id2));
    EXPECT_EQ(order_manager->getView(id2).value().quantity, 3);

    ASSERT_EQ(level.getOrders().size(), 1u);
    EXPECT_EQ(level.getOrders().front(), id2);

    // One trade per resting order touched, in the order they were consumed.
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].sell_id, id1);       // resting side is SELL here
    EXPECT_EQ(trades[0].buy_id, kAggressor); // incoming side takes the other slot
    EXPECT_EQ(trades[0].trade_qty, 10);
    EXPECT_EQ(trades[0].trade_price, 100);
    EXPECT_EQ(trades[1].sell_id, id2);
    EXPECT_EQ(trades[1].trade_qty, 2);       // only the part that actually traded
}

TEST_F(BookLevelTest, FillOrdersExactlyEmptiesLevel) {
    BookLevel level(order_manager, 100);
    const order_id_t id = level.addOrder(OrderSide::SELL, OrderType::LIMIT, 100, 10);

    std::vector<TradeEvent> trades;
    EXPECT_EQ(level.fillOrders(10, trades, kAggressor), 0);
    EXPECT_EQ(level.getTotalQuantity(), 0);
    EXPECT_FALSE(order_manager->valid(id));
    EXPECT_TRUE(level.getOrders().empty());

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].trade_qty, 10);
}

TEST_F(BookLevelTest, FillOrdersReturnsUnfilledRemainder) {
    BookLevel level(order_manager, 100);
    level.addOrder(OrderSide::SELL, OrderType::LIMIT, 100, 10);

    // Only 10 units are resting, so 5 of the requested 15 go unfilled.
    std::vector<TradeEvent> trades;
    EXPECT_EQ(level.fillOrders(15, trades, kAggressor), 5);
    EXPECT_EQ(level.getTotalQuantity(), 0);
    EXPECT_TRUE(level.getOrders().empty());

    // The unfilled remainder is not a trade.
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].trade_qty, 10);
}

TEST_F(BookLevelTest, FillOrdersOnEmptyLevelReportsNoTrades) {
    BookLevel level(order_manager, 100);

    std::vector<TradeEvent> trades;
    EXPECT_EQ(level.fillOrders(10, trades, kAggressor), 10);
    EXPECT_TRUE(trades.empty());
}

TEST_F(BookLevelTest, FillOrdersReportsBuyRestingOrderOnTheBuySlot) {
    BookLevel level(order_manager, 100);
    const order_id_t id = level.addOrder(OrderSide::BUY, OrderType::LIMIT, 100, 10);

    std::vector<TradeEvent> trades;
    EXPECT_EQ(level.fillOrders(10, trades, kAggressor), 0);

    // Mirror of the SELL case: the resting buy owns buy_id, the aggressor sell_id.
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buy_id, id);
    EXPECT_EQ(trades[0].sell_id, kAggressor);
}
