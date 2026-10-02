#include "ReplayValidator.h"

#include <stdexcept>
#include <utility>

namespace {

struct Top {
    int32_t px = mbo::PRICE_UNDEF;
    uint32_t sz = 0;
};

Top top(const OrderBook& book, OrderSide side) {
    const auto level = book.getTopLevel(side);
    if (!level) return {};
    return {level->price, static_cast<uint32_t>(level->quantity)};
}

// mbp-1 reports an empty side with an undefined price; its size there is not meaningful.
bool sameSide(Top book, int32_t venue_px, uint32_t venue_sz) {
    if (book.px != venue_px) return false;
    return venue_px == mbo::PRICE_UNDEF || book.sz == venue_sz;
}

using Key = std::pair<uint64_t, uint32_t>;  // (ts_recv, sequence): records are ordered by it

}  // namespace

ValidationReport validateAgainstMbp1(const MboEvents& mbo, const Mbp1Events& mbp, size_t max_examples) {
    if (!mbo.consistent() || !mbp.consistent()) {
        throw std::invalid_argument("validateAgainstMbp1: columns of different lengths");
    }

    ValidationReport report;
    MarketReplayer replayer;
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

        const Top bid = top(book, OrderSide::BUY);
        const Top ask = top(book, OrderSide::SELL);
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
            if (!sameSide(bid, mbp.bid_px[j], mbp.bid_sz[j]) || !sameSide(ask, mbp.ask_px[j], mbp.ask_sz[j])) {
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

    report.replay = replayer.getStats();
    return report;
}
