#include "OrderManager.h"

#include <gtest/gtest.h>

#include <set>
#include <vector>

// OrderManager is the single owner of every Order. Everything above it holds
// order ids and asks the manager for state, so the contract under test is:
//   - ids are unique and never reused, even though the slots behind them are;
//   - an order that is cancelled, fully filled, or modified to zero quantity is
//     RELEASED, after which getView() reports nullopt rather than stale data;
//   - operations on an unknown, dead, or stale id fail instead of throwing.
//
// State is read through getView(), which costs one slab index for all fields.

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
    // 0 is never minted, so it stays free as a not-found sentinel.
    EXPECT_EQ(ids.count(0), 0u);
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
    // its slot released — the same treatment cancel and fulfill give.
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

// --- Slab behaviour ---------------------------------------------------------
// Ids are (generation << 32 | slot index). The tests below pin down the parts of
// that scheme callers rely on without depending on the exact bit layout.

TEST(OrderManager, FreedSlotIsReusedUnderAFreshId) {
    OrderManager manager(1);
    const order_id_t first = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);
    ASSERT_TRUE(manager.cancel_order(first));

    // Capacity 1 and nothing live: the only way to place this order without
    // growing is to reuse the freed slot.
    const order_id_t second = manager.add_order(OrderSide::SELL, OrderType::LIMIT, 7, 101);
    EXPECT_EQ(manager.capacity(), 1u);
    EXPECT_NE(second, first);
}

TEST(OrderManager, StaleIdCannotTouchTheOrderNowInItsSlot) {
    OrderManager manager(1);
    const order_id_t stale = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);
    ASSERT_TRUE(manager.cancel_order(stale));
    const order_id_t fresh = manager.add_order(OrderSide::SELL, OrderType::LIMIT, 7, 101);

    // Every operation through the stale id must miss, and leave the new
    // occupant of the slot untouched.
    EXPECT_FALSE(manager.valid(stale));
    EXPECT_FALSE(manager.getView(stale).has_value());
    EXPECT_FALSE(manager.cancel_order(stale));
    EXPECT_FALSE(manager.modify_order(stale, 1, 99));
    EXPECT_EQ(manager.fulfill_order(stale, 3), 3);

    const auto view = manager.getView(fresh);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->orderId, fresh);
    EXPECT_EQ(view->side, OrderSide::SELL);
    EXPECT_EQ(view->quantity, 7);
    EXPECT_EQ(view->price, 101);
}

TEST(OrderManager, IdsStayUniqueUnderHeavySlotReuse) {
    OrderManager manager(4);
    std::set<order_id_t> ids;
    // Cycle the same few slots many times over; every id handed out must be new.
    for (int i = 0; i < 1000; ++i) {
        const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 1, 100);
        EXPECT_TRUE(ids.insert(id).second) << "id " << id << " reissued";
        ASSERT_EQ(manager.fulfill_order(id, 1), 0);
    }
    EXPECT_EQ(manager.capacity(), 4u);
}

TEST(OrderManager, IdOfANeverUsedSlotIsUnknown) {
    OrderManager manager(8);
    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);

    // Same generation, neighbouring slot that was never allocated.
    EXPECT_FALSE(manager.valid(id + 1));
    // Even generations mark free slots; no id built from one may resolve.
    EXPECT_FALSE(manager.valid(id & 0xFFFF'FFFFu));
    EXPECT_FALSE(manager.valid(0));
}

TEST(OrderManager, GrowingPastCapacityKeepsExistingOrders) {
    OrderManager manager(2);
    std::vector<order_id_t> ids;
    for (int i = 0; i < 50; ++i) {
        ids.push_back(manager.add_order(OrderSide::BUY, OrderType::LIMIT, i + 1, 100 + i));
    }
    EXPECT_GE(manager.capacity(), 50u);

    // Every order placed before (and across) each doubling is still intact.
    for (int i = 0; i < 50; ++i) {
        const auto view = manager.getView(ids[static_cast<size_t>(i)]);
        ASSERT_TRUE(view.has_value()) << "order " << i << " lost in growth";
        EXPECT_EQ(view->quantity, i + 1);
        EXPECT_EQ(view->price, 100 + i);
    }
}

TEST(OrderManager, ZeroCapacityStillAcceptsOrders) {
    OrderManager manager(0);
    const order_id_t id = manager.add_order(OrderSide::BUY, OrderType::LIMIT, 10, 100);
    EXPECT_TRUE(manager.valid(id));
}
