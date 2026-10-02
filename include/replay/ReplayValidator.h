#pragma once

#include "MarketReplayer.h"
#include "MboEvents.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

// One point where the rebuilt top of book disagreed with the venue's. A price of
// mbo::PRICE_UNDEF means that side was empty.
struct TopMismatch {
    uint64_t ts_recv;
    uint32_t sequence;
    int32_t book_bid_px, book_ask_px;
    uint32_t book_bid_sz, book_ask_sz;
    int32_t venue_bid_px, venue_ask_px;
    uint32_t venue_bid_sz, venue_ask_sz;
};

struct ValidationReport {
    ReplayStats replay;
    uint64_t compared = 0;          // event ends checked against an mbp-1 record
    uint64_t mismatched = 0;        // of those, how many disagreed on price or size
    uint64_t crossed = 0;           // event ends where the rebuilt book crossed (bid >= ask)
    uint64_t venue_unmatched = 0;   // mbp-1 F_LAST records with no MBO event end at their key
    std::vector<TopMismatch> examples;  // the first few mismatches
};

// Replays `mbo` from the start and, after every MBO event (F_LAST), compares the
// rebuilt best bid/ask, price and size, with Nasdaq's mbp-1 record for the
// same event.
//
// Alignment needs no guessing: every mbp-1 record carrying F_LAST has exactly
// the (ts_recv, sequence) of an MBO event's F_LAST record, and those keys are
// unique (measured on 2026-09-29: 809,283 of 809,283 for AAPL). Events that do
// not touch the top of book have no mbp-1 record; those are checked only for a
// crossed book.
ValidationReport validateAgainstMbp1(const MboEvents& mbo, const Mbp1Events& mbp, size_t max_examples = 20);

// The comparison loop, for any replayer with applyEvent(events, i) and
// getBook(). report.replay is left for the caller to fill.
template <typename Replayer>
ValidationReport validateWith(Replayer& replayer, const MboEvents& mbo, const Mbp1Events& mbp, size_t max_examples);

namespace replay_detail {

struct Top {
    int32_t px = mbo::PRICE_UNDEF;
    uint32_t sz = 0;
};

inline Top top(const OrderBook& book, OrderSide side) {
    const auto level = book.getTopLevel(side);
    if (!level) return {};
    return {level->price, static_cast<uint32_t>(level->quantity)};
}

// mbp-1 reports an empty side with an undefined price; its size there is not meaningful.
inline bool sameSide(Top book, int32_t venue_px, uint32_t venue_sz) {
    if (book.px != venue_px) return false;
    return venue_px == mbo::PRICE_UNDEF || book.sz == venue_sz;
}

}  // namespace replay_detail

template <typename Replayer>
ValidationReport validateWith(Replayer& replayer, const MboEvents& mbo, const Mbp1Events& mbp, size_t max_examples) {
    using replay_detail::Top;
    using Key = std::pair<uint64_t, uint32_t>;  // (ts_recv, sequence): records are ordered by it

    if (!mbo.consistent() || !mbp.consistent()) {
        throw std::invalid_argument("validate: columns of different lengths");
    }

    ValidationReport report;
    const OrderBook& book = replayer.getBook();

    // Next mbp-1 record carrying F_LAST.
    size_t j = 0;
    auto skipToLast = [&] {
        while (j < mbp.count() && !(mbp.flags[j] & mbo::F_LAST)) ++j;
    };
    skipToLast();

    size_t i = 0;
    while (i < mbo.count()) {
        i = replayer.applyEvent(mbo, i);
        const size_t end = i - 1;
        if (!(mbo.flags[end] & mbo::F_LAST)) break;   // trailing records with no F_LAST
        const Key key{mbo.ts_recv[end], mbo.sequence[end]};

        const Top bid = replay_detail::top(book, OrderSide::BUY);
        const Top ask = replay_detail::top(book, OrderSide::SELL);
        if (bid.px != mbo::PRICE_UNDEF && ask.px != mbo::PRICE_UNDEF && bid.px >= ask.px) {
            report.crossed += 1;
        }

        // mbp-1 records whose key is behind this event matched no MBO event end.
        while (j < mbp.count() && Key{mbp.ts_recv[j], mbp.sequence[j]} < key) {
            report.venue_unmatched += 1;
            ++j;
            skipToLast();
        }
        if (j < mbp.count() && Key{mbp.ts_recv[j], mbp.sequence[j]} == key) {
            report.compared += 1;
            if (!replay_detail::sameSide(bid, mbp.bid_px[j], mbp.bid_sz[j]) ||
                !replay_detail::sameSide(ask, mbp.ask_px[j], mbp.ask_sz[j])) {
                report.mismatched += 1;
                if (report.examples.size() < max_examples) {
                    report.examples.push_back({key.first, key.second,
                                               bid.px, ask.px, bid.sz, ask.sz,
                                               mbp.bid_px[j], mbp.ask_px[j], mbp.bid_sz[j], mbp.ask_sz[j]});
                }
            }
            ++j;
            skipToLast();
        }
    }
    while (j < mbp.count()) {
        report.venue_unmatched += 1;
        ++j;
        skipToLast();
    }
    return report;
}
