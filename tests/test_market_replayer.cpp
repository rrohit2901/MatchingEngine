#include "MarketReplayer.h"
#include "ReplayValidator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

// The replay applies Databento MBO records to an OrderBook. These tests feed it
// hand-written sequences shaped like what Phase 0 measured on real Nasdaq data
// (docs/strategy-replay-plan.md): executions arrive as T, then F, then the C that
// removes the filled size; a replace is C + A with a new id; there are no M
// records on XNAS.ITCH, but M is still supported.

namespace {

// Owns the columns that MboEvents / Mbp1Events only view.
struct Tape {
    std::vector<uint64_t> ts, oid;
    std::vector<uint8_t> action, side, flags;
    std::vector<int32_t> price;
    std::vector<uint32_t> size, seq;

    // One record. `last` marks the end of a venue event (F_LAST).
    Tape& rec(char a, char s, int32_t px, uint32_t sz, uint64_t id, bool last = true) {
        ts.push_back(1000 + ts.size());
        seq.push_back(static_cast<uint32_t>(seq.size()));
        action.push_back(static_cast<uint8_t>(a));
        side.push_back(static_cast<uint8_t>(s));
        price.push_back(px);
        size.push_back(sz);
        oid.push_back(id);
        flags.push_back(last ? mbo::F_LAST : uint8_t{0});
        return *this;
    }

    MboEvents events() const { return {ts, action, side, price, size, oid, flags, seq}; }
};

struct VenueTops {
    std::vector<uint64_t> ts;
    std::vector<uint32_t> seq, bid_sz, ask_sz;
    std::vector<uint8_t> flags;
    std::vector<int32_t> bid_px, ask_px;

    VenueTops& at(const Tape& tape, size_t record, int32_t bpx, uint32_t bsz, int32_t apx, uint32_t asz) {
        ts.push_back(tape.ts[record]);
        seq.push_back(tape.seq[record]);
        flags.push_back(mbo::F_LAST);
        bid_px.push_back(bpx); bid_sz.push_back(bsz);
        ask_px.push_back(apx); ask_sz.push_back(asz);
        return *this;
    }

    Mbp1Events events() const { return {ts, seq, flags, bid_px, ask_px, bid_sz, ask_sz}; }
};

void replayAll(MarketReplayer& replayer, const MboEvents& events) {
    for (size_t i = 0; i < events.count();) i = replayer.applyEvent(events, i);
}

int topQty(const MarketReplayer& r, OrderSide side) {
    const auto level = r.getBook().getTopLevel(side);
    return level ? level->quantity : 0;
}

std::optional<int> topPx(const MarketReplayer& r, OrderSide side) {
    return r.getBook().getBestPrice(side);
}

}  // namespace

TEST(MarketReplayer, AddsRestWithoutMatching) {
    Tape tape;
    // A crossed pair from the feed must still rest: the venue did the matching.
    tape.rec('A', 'B', 1000, 10, 1).rec('A', 'A', 1001, 5, 2);
    MarketReplayer r;
    replayAll(r, tape.events());

    EXPECT_EQ(topPx(r, OrderSide::BUY), 1000);
    EXPECT_EQ(topPx(r, OrderSide::SELL), 1001);
    EXPECT_EQ(r.liveOrders(), 2u);
    EXPECT_EQ(r.getStats().adds, 2u);
}

TEST(MarketReplayer, CancelSizeIsTheAmountRemoved) {
    Tape tape;
    tape.rec('A', 'B', 1000, 10, 1)
        .rec('C', 'B', 1000, 3, 1)    // partial: 10 -> 7
        .rec('C', 'B', 1000, 7, 1);   // the rest
    MarketReplayer r;
    auto events = tape.events();

    size_t i = r.applyEvent(events, 0);
    i = r.applyEvent(events, i);
    EXPECT_EQ(topQty(r, OrderSide::BUY), 7);
    EXPECT_EQ(r.getStats().cancels_partial, 1u);

    r.applyEvent(events, i);
    EXPECT_FALSE(topPx(r, OrderSide::BUY).has_value());
    EXPECT_EQ(r.getStats().cancels_full, 1u);
    EXPECT_EQ(r.liveOrders(), 0u);
}

TEST(MarketReplayer, PartialCancelKeepsQueuePriority) {
    Tape tape;
    tape.rec('A', 'B', 1000, 10, 1)
        .rec('A', 'B', 1000, 5, 2)
        .rec('C', 'B', 1000, 4, 1);   // order 1 shrinks but stays first in the queue
    MarketReplayer r;
    const order_id_t first = [&] {
        auto events = tape.events();
        r.applyEvent(events, 0);
        return r.engineId(1);
    }();
    auto events = tape.events();
    r.applyEvent(events, 1);
    r.applyEvent(events, 2);

    EXPECT_EQ(r.engineId(1), first);   // same engine id: not re-queued
    EXPECT_EQ(topQty(r, OrderSide::BUY), 11);
}

TEST(MarketReplayer, ExecutionIsRemovedByTheCancelNotTheFill) {
    Tape tape;
    tape.rec('A', 'A', 1001, 10, 7)
        // A buy aggressor takes 4: T and F change nothing, the C removes the 4.
        .rec('T', 'B', 1001, 4, 0, false)
        .rec('F', 'A', 1001, 4, 7, false)
        .rec('C', 'A', 1001, 4, 7, true);
    MarketReplayer r;
    auto events = tape.events();
    size_t i = r.applyEvent(events, 0);
    EXPECT_EQ(topQty(r, OrderSide::SELL), 10);

    i = r.applyEvent(events, i);
    EXPECT_EQ(i, events.count());   // the whole T F C event in one call
    EXPECT_EQ(topQty(r, OrderSide::SELL), 6);
    EXPECT_EQ(r.getStats().trades, 1u);
    EXPECT_EQ(r.getStats().fills, 1u);
    EXPECT_EQ(r.getStats().unknown_order, 0u);
}

TEST(MarketReplayer, ReplaceIsCancelPlusAddWithNewId) {
    Tape tape;
    tape.rec('A', 'B', 1000, 10, 1)
        .rec('C', 'B', 1000, 10, 1, false)
        .rec('A', 'B', 1002, 10, 2, true);
    MarketReplayer r;
    replayAll(r, tape.events());

    EXPECT_EQ(topPx(r, OrderSide::BUY), 1002);
    EXPECT_EQ(r.engineId(1), 0u);
    EXPECT_NE(r.engineId(2), 0u);
}

TEST(MarketReplayer, ModifyFollowsReissuedEngineIds) {
    Tape tape;
    tape.rec('A', 'B', 1000, 10, 1)
        .rec('M', 'B', 1001, 10, 1)   // price change: re-queued under a new engine id
        .rec('C', 'B', 1001, 10, 1);  // must still find it through the venue id
    MarketReplayer r;
    auto events = tape.events();
    size_t i = r.applyEvent(events, 0);
    const order_id_t before = r.engineId(1);
    i = r.applyEvent(events, i);
    EXPECT_NE(r.engineId(1), before);
    EXPECT_EQ(topPx(r, OrderSide::BUY), 1001);

    r.applyEvent(events, i);
    EXPECT_EQ(r.liveOrders(), 0u);
    EXPECT_EQ(r.getStats().unknown_order, 0u);
}

TEST(MarketReplayer, ModifyOfUnknownOrderIsAnAdd) {
    Tape tape;
    tape.rec('M', 'A', 1005, 3, 42);
    MarketReplayer r;
    replayAll(r, tape.events());
    EXPECT_EQ(topPx(r, OrderSide::SELL), 1005);
    EXPECT_EQ(r.getStats().modify_unknown, 1u);
}

TEST(MarketReplayer, ClearEmptiesTheBook) {
    Tape tape;
    tape.rec('A', 'B', 1000, 10, 1).rec('A', 'A', 1001, 5, 2).rec('R', 'N', mbo::PRICE_UNDEF, 0, 0);
    MarketReplayer r;
    replayAll(r, tape.events());
    EXPECT_FALSE(topPx(r, OrderSide::BUY).has_value());
    EXPECT_FALSE(topPx(r, OrderSide::SELL).has_value());
    EXPECT_EQ(r.liveOrders(), 0u);
}

TEST(MarketReplayer, AnomaliesAreCountedNotApplied) {
    Tape tape;
    tape.rec('C', 'B', 1000, 5, 99)          // unknown id
        .rec('A', 'N', 1000, 5, 3)           // no side
        .rec('A', 'B', mbo::PRICE_UNDEF, 5, 4)
        .rec('A', 'B', 1000, 5, 5)
        .rec('C', 'B', 1000, 9, 5);          // larger than the order
    MarketReplayer r;
    replayAll(r, tape.events());
    const auto& s = r.getStats();
    EXPECT_EQ(s.unknown_order, 1u);
    EXPECT_EQ(s.bad_side, 1u);
    EXPECT_EQ(s.bad_price, 1u);
    EXPECT_EQ(s.cancel_oversized, 1u);
    EXPECT_EQ(r.liveOrders(), 0u);
}

TEST(ReplayValidator, MatchesVenueTopOfBook) {
    Tape tape;
    tape.rec('A', 'B', 1000, 10, 1)
        .rec('A', 'A', 1002, 4, 2)
        .rec('T', 'B', 1002, 4, 0, false).rec('F', 'A', 1002, 4, 2, false).rec('C', 'A', 1002, 4, 2, true)
        .rec('A', 'B', 999, 1, 3);   // below the top: no mbp-1 record for this event
    VenueTops venue;
    venue.at(tape, 0, 1000, 10, mbo::PRICE_UNDEF, 0)
         .at(tape, 1, 1000, 10, 1002, 4)
         .at(tape, 4, 1000, 10, mbo::PRICE_UNDEF, 0);

    const auto report = validateAgainstMbp1(tape.events(), venue.events());
    EXPECT_EQ(report.compared, 3u);
    EXPECT_EQ(report.mismatched, 0u);
    EXPECT_EQ(report.venue_unmatched, 0u);
    EXPECT_EQ(report.crossed, 0u);
}

TEST(ReplayValidator, ReportsMismatchesAndUnmatchedVenueRecords) {
    Tape tape;
    tape.rec('A', 'B', 1000, 10, 1).rec('A', 'A', 1001, 5, 2);
    VenueTops venue;
    venue.at(tape, 0, 1000, 9, mbo::PRICE_UNDEF, 0)   // wrong size
         .at(tape, 1, 1000, 10, 1001, 5);
    venue.ts.push_back(5000); venue.seq.push_back(99); venue.flags.push_back(mbo::F_LAST);   // no such event
    venue.bid_px.push_back(1000); venue.bid_sz.push_back(10); venue.ask_px.push_back(1001); venue.ask_sz.push_back(5);

    const auto report = validateAgainstMbp1(tape.events(), venue.events());
    EXPECT_EQ(report.compared, 2u);
    EXPECT_EQ(report.mismatched, 1u);
    ASSERT_EQ(report.examples.size(), 1u);
    EXPECT_EQ(report.examples[0].book_bid_sz, 10u);
    EXPECT_EQ(report.examples[0].venue_bid_sz, 9u);
    EXPECT_EQ(report.venue_unmatched, 1u);
}

TEST(ReplayValidator, DetectsCrossedBook) {
    Tape tape;
    tape.rec('A', 'B', 1001, 1, 1).rec('A', 'A', 1000, 1, 2);
    const auto report = validateAgainstMbp1(tape.events(), VenueTops{}.events());
    EXPECT_EQ(report.crossed, 1u);
}
