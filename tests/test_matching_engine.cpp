#include "MatchingEngine.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

// MatchingEngine is OrderBook plus automatic matching: every addOrder and
// modifyOrder runs the Matcher, so a crossing order trades on arrival instead of
// resting. It exposes no per-order accessors, so these tests observe the book
// through the level views.

namespace {

using Levels = std::vector<std::shared_ptr<BookLevel>>;

int restingQuantity(const Levels& levels) {
    int total = 0;
    for (const auto& level : levels) {
        total += level->getTotalQuantity();
    }
    return total;
}

std::size_t restingOrders(const Levels& levels) {
    std::size_t count = 0;
    for (const auto& level : levels) {
        count += level->getOrders().size();
    }
    return count;
}

} // namespace

TEST(MatchingEngine, EmptyBook) {
    MatchingEngine me;
    EXPECT_TRUE(me.getBuySideView(5).empty());
    EXPECT_TRUE(me.getSellSideView(5).empty());

    const auto [bids, asks] = me.getOrderBookView(5);
    EXPECT_TRUE(bids.empty());
    EXPECT_TRUE(asks.empty());
}

TEST(MatchingEngine, NonCrossingOrdersRest) {
    MatchingEngine me;
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(101, 5, OrderType::LIMIT, OrderSide::SELL);

    const auto bids = me.getBuySideView(5);
    ASSERT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids.front()->getPrice(), 100);
    EXPECT_EQ(bids.front()->getTotalQuantity(), 10);

    const auto asks = me.getSellSideView(5);
    ASSERT_EQ(asks.size(), 1u);
    EXPECT_EQ(asks.front()->getPrice(), 101);
    EXPECT_EQ(asks.front()->getTotalQuantity(), 5);
}

TEST(MatchingEngine, CrossingOrderTradesOnArrival) {
    MatchingEngine me;
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    // Both sides are fully consumed, so no quantity and no live order remains.
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 0);
    EXPECT_EQ(restingOrders(me.getBuySideView(5)), 0u);
    EXPECT_EQ(restingOrders(me.getSellSideView(5)), 0u);
}

TEST(MatchingEngine, PartialFillLeavesRemainderResting) {
    MatchingEngine me;
    me.addOrder(100, 4, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    // The 4-lot ask is consumed; 6 of the buy's 10 stay on the bid at 100.
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 6);
    EXPECT_EQ(restingOrders(me.getBuySideView(5)), 1u);
}

TEST(MatchingEngine, RestingOrderAbsorbsSmallerAggressor) {
    MatchingEngine me;
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 4, OrderType::LIMIT, OrderSide::BUY);

    // The aggressing buy is fully filled; the ask keeps its 6 unfilled lots.
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 6);
}

TEST(MatchingEngine, IncomingSellCrossesRestingBid) {
    MatchingEngine me;
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);

    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 0);
}

TEST(MatchingEngine, OrderDoesNotTradeThroughItsLimit) {
    MatchingEngine me;
    me.addOrder(102, 10, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    // 100 does not reach a 102 ask, so both orders rest untouched.
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 10);
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 10);
}

TEST(MatchingEngine, AggressorSweepsBestPriceFirst) {
    MatchingEngine me;
    me.addOrder(102, 5, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 5, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(102, 5, OrderType::LIMIT, OrderSide::BUY);

    // The buy can pay up to 102 but only needs 5 lots, so it takes the 100 ask
    // and leaves the 102 ask alone.
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);

    const auto asks = me.getSellSideView(5);
    EXPECT_EQ(restingQuantity(asks), 5);
    ASSERT_FALSE(asks.empty());
    EXPECT_EQ(asks.back()->getPrice(), 102);
}

TEST(MatchingEngine, AggressorSweepsMultipleLevels) {
    MatchingEngine me;
    me.addOrder(100, 5, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(101, 5, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(101, 8, OrderType::LIMIT, OrderSide::BUY);

    // 8 lots clear the 100 level entirely and 3 of the 101 level.
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 2);
}

TEST(MatchingEngine, CancelOrder) {
    MatchingEngine me;
    const order_id_t id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    EXPECT_TRUE(me.cancelOrder(id));
    EXPECT_TRUE(me.getBuySideView(5).empty());
    EXPECT_FALSE(me.cancelOrder(id)); // already gone
}

TEST(MatchingEngine, CancelUnknownOrderFails) {
    MatchingEngine me;
    EXPECT_FALSE(me.cancelOrder(9999));
}

TEST(MatchingEngine, ModifyOrderRequotesToNewPrice) {
    MatchingEngine me;
    const order_id_t id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    const auto requoted = me.modifyOrder(id, 10, 99, OrderSide::BUY, OrderType::LIMIT);
    ASSERT_TRUE(requoted.has_value());

    const auto bids = me.getBuySideView(5);
    ASSERT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids.front()->getPrice(), 99);
    EXPECT_EQ(bids.front()->getTotalQuantity(), 10);
}

TEST(MatchingEngine, ModifyUnknownOrderReturnsNullopt) {
    MatchingEngine me;
    EXPECT_FALSE(me.modifyOrder(9999, 10, 100, OrderSide::BUY, OrderType::LIMIT).has_value());
}

TEST(MatchingEngine, ModifyIntoACrossTrades) {
    MatchingEngine me;
    me.addOrder(101, 10, OrderType::LIMIT, OrderSide::SELL);
    const order_id_t id = me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);
    ASSERT_EQ(restingQuantity(me.getSellSideView(5)), 10); // no cross yet

    // Repricing the bid up to the ask must match, not just move the order.
    const auto requoted = me.modifyOrder(id, 10, 101, OrderSide::BUY, OrderType::LIMIT);
    ASSERT_TRUE(requoted.has_value());
    EXPECT_EQ(restingQuantity(me.getSellSideView(5)), 0);
    EXPECT_EQ(restingQuantity(me.getBuySideView(5)), 0);
}

TEST(MatchingEngine, BookViewIsPriceOrderedAndCapped) {
    MatchingEngine me;
    me.addOrder(98, 1, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(100, 1, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(99, 1, OrderType::LIMIT, OrderSide::BUY);
    me.addOrder(103, 1, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(101, 1, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(102, 1, OrderType::LIMIT, OrderSide::SELL);

    const auto [bids, asks] = me.getOrderBookView(3);
    ASSERT_EQ(bids.size(), 3u);
    ASSERT_EQ(asks.size(), 3u);
    EXPECT_EQ(bids[0]->getPrice(), 100); // best bid is the highest
    EXPECT_EQ(bids[1]->getPrice(), 99);
    EXPECT_EQ(bids[2]->getPrice(), 98);
    EXPECT_EQ(asks[0]->getPrice(), 101); // best ask is the lowest
    EXPECT_EQ(asks[1]->getPrice(), 102);
    EXPECT_EQ(asks[2]->getPrice(), 103);

    EXPECT_EQ(me.getBuySideView(2).size(), 2u);
    EXPECT_EQ(me.getSellSideView(1).size(), 1u);
}

TEST(MatchingEngine, FullyFilledAggressorLeavesNoEmptyLevel) {
    MatchingEngine me;
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::SELL);
    me.addOrder(100, 10, OrderType::LIMIT, OrderSide::BUY);

    // A level with nothing resting on it must not occupy a slot in the depth
    // view, otherwise getOrderBookView(n) reports phantom depth.
    EXPECT_TRUE(me.getSellSideView(5).empty());
    EXPECT_TRUE(me.getBuySideView(5).empty());
}
