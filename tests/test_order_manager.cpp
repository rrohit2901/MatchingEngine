#include "OrderManager.h"

#include <gtest/gtest.h>

#include <set>

// OrderManager is the single owner of every Order. Everything above it holds
// order ids and asks the manager for state, so the contract under test is:
//   - ids are unique and never reused;
//   - an order that is cancelled, fully filled, or modified to zero quantity is
//     ERASED, after which getView() reports nullopt rather than stale data;
//   - operations on an unknown or dead id fail instead of throwing.
//
// State is read through getView(), which costs one hash lookup for all fields.

TEST(OrderManager, AddOrderExposesItsFields) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);

    ASSERT_TRUE(manager.valid(id));
    const auto view = manager.getView(id);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->orderId, id);
    EXPECT_EQ(view->side, OrderSide::BUY);
    EXPECT_EQ(view->type, OrderType::LIMIT);
    EXPECT_EQ(view->quantity, 10);
    EXPECT_EQ(view->price, 100);
}

TEST(OrderManager, ViewIsASnapshotNotALiveHandle) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);

    const auto before = manager.getView(id).value();
    ASSERT_TRUE(manager.modify_order(id, 4, 101));

    // The copy taken earlier must not track the later modification.
    EXPECT_EQ(before.quantity, 10);
    EXPECT_EQ(before.price, 100);
    EXPECT_EQ(manager.getView(id).value().quantity, 4);
    EXPECT_EQ(manager.getView(id).value().price, 101);
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

TEST(OrderManager, ViewOfUnknownIdIsNullopt) {
    OrderManager manager;
    EXPECT_FALSE(manager.valid(9999));
    EXPECT_FALSE(manager.getView(9999).has_value());
}

TEST(OrderManager, CancelOrderErasesIt) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);

    EXPECT_TRUE(manager.cancel_order(id));
    EXPECT_FALSE(manager.valid(id));
    EXPECT_FALSE(manager.getView(id).has_value());
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
    const auto view = manager.getView(id);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->quantity, 25);
    EXPECT_EQ(view->price, 101);
    // Side and type are not modifiable through this call.
    EXPECT_EQ(view->side, OrderSide::BUY);
    EXPECT_EQ(view->type, OrderType::LIMIT);
}

TEST(OrderManager, ModifyToZeroQuantityRetiresAndErasesOrder) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);

    EXPECT_TRUE(manager.modify_order(id, 0, 100));
    // A zero-quantity order counts as fulfilled, so it stops being valid AND is
    // dropped from the map — the same treatment cancel and fulfill give.
    EXPECT_FALSE(manager.valid(id));
    EXPECT_FALSE(manager.getView(id).has_value());
    // Erased, not merely tombstoned: a second modify finds nothing to act on.
    EXPECT_FALSE(manager.modify_order(id, 5, 100));
    EXPECT_FALSE(manager.cancel_order(id));
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
    ASSERT_TRUE(manager.valid(id));
    EXPECT_EQ(manager.getView(id).value().quantity, 6);
}

TEST(OrderManager, FulfillExactlyErasesOrder) {
    OrderManager manager;
    const order_id_t id = manager.add_order(OrderSide::SELL, OrderType::LIMIT, 10, 100);

    EXPECT_EQ(manager.fulfill_order(id, 10), 0);
    EXPECT_FALSE(manager.valid(id));
    EXPECT_FALSE(manager.getView(id).has_value());
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
    const auto view = manager.getView(sell);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->side, OrderSide::SELL);
    EXPECT_EQ(view->quantity, 5);
    EXPECT_EQ(view->price, 101);
}
