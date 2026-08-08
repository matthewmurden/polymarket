#include "wallet_segment.hpp"

#include <algorithm>

namespace {

// Trades/day since first_seen_unix, with a minimum 1-day floor on the
// elapsed-time denominator. Without this floor, a wallet with e.g. 3
// trades within the same minute would compute an astronomical trades/day
// figure and get misclassified as High despite having almost no real
// history -- a 1-day floor treats "everything we've seen happened within
// less than a day" as exactly that, not as an extrapolated annual rate.
//
// A wallet_history_fetch.hpp backfill-cap-truncated first_seen_unix (see
// the header doc comment) only ever makes this estimate LARGER than the
// true rate would be (fewer elapsed days for the same trade count), never
// smaller -- so the bias that makes age_score misleading for capped
// wallets pushes classification in the safe direction here: toward
// (correctly) calling them High, not away from it.
double tradesPerDay(const WalletHistoryRecord& h, uint64_t nowUnix) {
    if (h.trade_count == 0 || h.first_seen_unix == 0 || nowUnix <= h.first_seen_unix) {
        return 0.0;
    }
    double elapsedDays = static_cast<double>(nowUnix - h.first_seen_unix) / 86400.0;
    elapsedDays = std::max(elapsedDays, 1.0);
    return static_cast<double>(h.trade_count) / elapsedDays;
}

} // namespace

std::string tierToString(WalletFrequencyTier tier) {
    switch (tier) {
        case WalletFrequencyTier::Low: return "low";
        case WalletFrequencyTier::Medium: return "medium";
        case WalletFrequencyTier::High: return "high";
    }
    return "unknown";
}

WalletFrequencyTier classifyWallet(const WalletHistoryRecord& priorHistory, uint64_t nowUnix,
                                    const WalletSegmentConfig& cfg) {
    double rate = tradesPerDay(priorHistory, nowUnix);

    // High check first: trade_count alone is decisive for a wallet that's
    // already hit the backfill cap (see header), independent of the rate
    // estimate.
    if (priorHistory.trade_count >= cfg.highMinTrades || rate >= cfg.highMinTradesPerDay) {
        return WalletFrequencyTier::High;
    }
    if (priorHistory.trade_count <= cfg.lowMaxTrades && rate <= cfg.lowMaxTradesPerDay) {
        return WalletFrequencyTier::Low;
    }
    return WalletFrequencyTier::Medium;
}
