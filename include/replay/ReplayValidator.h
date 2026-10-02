#pragma once

#include "MarketReplayer.h"
#include "MboEvents.h"

#include <cstddef>
#include <cstdint>
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
