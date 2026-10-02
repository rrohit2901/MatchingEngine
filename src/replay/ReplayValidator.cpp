#include "ReplayValidator.h"

ValidationReport validateAgainstMbp1(const MboEvents& mbo, const Mbp1Events& mbp, size_t max_examples) {
    MarketReplayer replayer;
    ValidationReport report = validateWith(replayer, mbo, mbp, max_examples);
    report.replay = replayer.getStats();
    return report;
}
