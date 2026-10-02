#pragma once

#include "MboEvents.h"
#include "OrderBook.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

// What the replay did with each kind of record, plus every record it could not
// apply cleanly. On a clean day the anomaly counters stay at zero; see
// docs/strategy-replay-plan.md (Phase 0 results) for what was measured.
struct ReplayStats {
    uint64_t records = 0;
    uint64_t events = 0;            // records carrying F_LAST
    uint64_t adds = 0;
    uint64_t cancels_full = 0;
    uint64_t cancels_partial = 0;
    uint64_t modifies = 0;
    uint64_t clears = 0;
    uint64_t trades = 0;            // T: no effect on the book
    uint64_t fills = 0;             // F: no effect on the book; the C that follows removes the size
    uint64_t nones = 0;

    // Anomalies: records the replay skipped or had to interpret.
    uint64_t unknown_order = 0;     // C, or F, for an order id that is not resting
    uint64_t modify_unknown = 0;    // M for an unknown id: applied as an add
    uint64_t duplicate_add = 0;     // A for an id that is already resting: replaces it
    uint64_t cancel_oversized = 0;  // C larger than the order: cancelled in full
    uint64_t bad_side = 0;          // A or M with a side other than B or A
    uint64_t bad_price = 0;         // A or M with PRICE_UNDEF
    uint64_t unknown_action = 0;
};

// Rebuilds the venue's order book from MBO records.
//
// Market records go straight to OrderBook: OrderBook::addOrder rests an order
// without matching it and without any risk check, which is exactly right for a
// feed. The venue has already matched everything, and its executions arrive
// as T and F records (which change nothing) followed by the C that takes the
// filled size off the book.
//
// Databento order ids are the venue's; OrderBook mints its own. ids maps one to
// the other and is kept in step whenever the engine reissues an id (a modify
// that increases size or changes price loses queue priority and gets a new id).
class MarketReplayer {
    private:
        OrderBook book;
        std::unordered_map<uint64_t, order_id_t> ids;
        ReplayStats stats;

        void add(uint64_t venue_id, OrderSide side, int price, int size);
        void cancel(uint64_t venue_id, int size);
        void modify(uint64_t venue_id, OrderSide side, int price, int size);
        void clear();

    public:
        // expected_orders sizes the id map so it does not rehash mid-replay.
        explicit MarketReplayer(size_t expected_orders = size_t{1} << 18);

        // Applies record i.
        void apply(const MboEvents& events, size_t i);
        // Applies records from i up to and including the next F_LAST record (or
        // the end), and returns the index after it.
        size_t applyEvent(const MboEvents& events, size_t i);

        const OrderBook& getBook() const { return book; }
        const ReplayStats& getStats() const { return stats; }
        size_t liveOrders() const { return ids.size(); }
        // The engine id for a venue order id, or 0 (never a live id) if it is not resting.
        order_id_t engineId(uint64_t venue_id) const;
};
