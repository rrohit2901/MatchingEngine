#include "OrderManager.h"

#include <gtest/gtest.h>

#include <set>

// OrderManager is the single owner of every Order. Everything above it holds
// order ids and asks the manager for state, so the contract under test is:
//   - ids are unique and never reused;
//   - an order that is cancelled or fully filled is ERASED, after which every
//     accessor reports nullopt / false rather than stale data;
//   - operations on an unknown or dead id fail instead of throwing.

TEST(OrderManager, AddOrderExposesItsFields) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);

    EXPECT_TRUE(manager.valid(id));
    EXPECT_EQ(manager.getSide(id).value(), OrderSide::BUY);
    EXPECT_EQ(manager.getType(id).value(), OrderType::LIMIT);
    EXPECT_EQ(manager.getQuantity(id).value(), 10);
    EXPECT_EQ(manager.getPrice(id).value(), 100);
}

TEST(OrderManager, AddOrderIssuesUniqueIds) {
    OrderManager manager;
    std::set<order_id_t> ids;
    for (int i = 0; i < 100; ++i) {
        ids.insert(manager.add_order(OrderSide::SELL, OrderType::LIMIT, 1, 100));
    }
    EXPECT_EQ(ids.size(), 100u);
    // Ids start at 1, so 0 stays free as a not-found sentinel.
    EXPECT_EQ(*ids.begin(), 1u);
}

TEST(OrderManager, IdsAreNotReusedAfterCancel) {
    OrderManager manager;
    const order_id_t first = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);
    ASSERT_TRUE(manager.cancel_order(first));

    const order_id_t second = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);
    EXPECT_NE(second, first);
}

TEST(OrderManager, AccessorsOnUnknownIdReturnNullopt) {
    OrderManager manager;
    EXPECT_FALSE(manager.valid(9999));
    EXPECT_FALSE(manager.getSide(9999).has_value());
    EXPECT_FALSE(manager.getType(9999).has_value());
    EXPECT_FALSE(manager.getPrice(9999).has_value());
    EXPECT_FALSE(manager.getQuantity(9999).has_value());
}

TEST(OrderManager, CancelOrderErasesIt) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);

    EXPECT_TRUE(manager.cancel_order(id));
    EXPECT_FALSE(manager.valid(id));
    EXPECT_FALSE(manager.getSide(id).has_value());
    EXPECT_FALSE(manager.getQuantity(id).has_value());
}

TEST(OrderManager, CancelOrderTwiceFails) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);

    ASSERT_TRUE(manager.cancel_order(id));
    EXPECT_FALSE(manager.cancel_order(id));
}

TEST(OrderManager, CancelUnknownOrderFails) {
    OrderManager manager;
    EXPECT_FALSE(manager.cancel_order(9999));
}

TEST(OrderManager, ModifyOrderUpdatesQuantityAndPrice) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);

    EXPECT_TRUE(manager.modify_order(id, 25, 101));
    EXPECT_EQ(manager.getQuantity(id).value(), 25);
    EXPECT_EQ(manager.getPrice(id).value(), 101);
    // Side and type are not modifiable through this call.
    EXPECT_EQ(manager.getSide(id).value(), OrderSide::BUY);
    EXPECT_EQ(manager.getType(id).value(), OrderType::LIMIT);
}

TEST(OrderManager, ModifyToZeroQuantityRetiresOrder) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);

    EXPECT_TRUE(manager.modify_order(id, 0, 100));
    // A zero-quantity order counts as fulfilled, so it stops being valid.
    EXPECT_FALSE(manager.valid(id));
    EXPECT_FALSE(manager.getQuantity(id).has_value());
}

TEST(OrderManager, ModifyUnknownOrCancelledOrderFails) {
    OrderManager manager;
    EXPECT_FALSE(manager.modify_order(9999, 10, 100));

    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);
    ASSERT_TRUE(manager.cancel_order(id));
    EXPECT_FALSE(manager.modify_order(id, 5, 100));
}

TEST(OrderManager, FulfillPartiallyLeavesOrderResting) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::SELL, OrderType::LIMIT, 10, 100);

    // 4 units of the incoming 4 are absorbed, so nothing is left over.
    EXPECT_EQ(manager.fulfill_order(id, 4), 0);
    EXPECT_TRUE(manager.valid(id));
    EXPECT_EQ(manager.getQuantity(id).value(), 6);
}

TEST(OrderManager, FulfillExactlyErasesOrder) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::SELL, OrderType::LIMIT, 10, 100);

    EXPECT_EQ(manager.fulfill_order(id, 10), 0);
    EXPECT_FALSE(manager.valid(id));
    EXPECT_FALSE(manager.getQuantity(id).has_value());
}

TEST(OrderManager, FulfillReturnsUnabsorbedRemainder) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::SELL, OrderType::LIMIT, 10, 100);

    // Only 10 of the incoming 15 fit, so 5 come back for the next level.
    EXPECT_EQ(manager.fulfill_order(id, 15), 5);
    EXPECT_FALSE(manager.valid(id));
}

TEST(OrderManager, FulfillUnknownOrderPassesQuantityThrough) {
    OrderManager manager;
    // An unknown id absorbs nothing, so the caller gets its full quantity back.
    EXPECT_EQ(manager.fulfill_order(9999, 7), 7);
}

TEST(OrderManager, FulfillDeadOrderPassesQuantityThrough) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::SELL, OrderType::LIMIT, 10, 100);
    ASSERT_TRUE(manager.cancel_order(id));

    EXPECT_EQ(manager.fulfill_order(id, 7), 7);
}

TEST(OrderManager, OrdersAreIndependent) {
    OrderManager manager;
    const order_id_t buy = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);
    const order_id_t sell = manager.add_order(OrderSide::SELL, OrderType::LIMIT, 5, 101);

    ASSERT_TRUE(manager.cancel_order(buy));
    EXPECT_FALSE(manager.valid(buy));

    // Cancelling one order must not disturb any other.
    ASSERT_TRUE(manager.valid(sell));
    EXPECT_EQ(manager.getSide(sell).value(), OrderSide::SELL);
    EXPECT_EQ(manager.getQuantity(sell).value(), 5);
    EXPECT_EQ(manager.getPrice(sell).value(), 101);
}
