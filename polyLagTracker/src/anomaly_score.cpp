#include "anomaly_score.hpp"

#include <algorithm>
#include <map>

namespace {

double clamp01(double v) {
    return std::max(0.0, std::min(1.0, v));
}

// this trade's size vs. the wallet's PRIOR trades only (see header for why
// prior, not post-fold). With fewer than 2 prior trades there's no real
// stddev to measure against (0 prior trades: no baseline at all; 1 prior
// trade: stddev is mathematically undefined) -- in both cases this falls
// back to treating 10% of the prior mean as a stand-in "typical spread".
// With 0 prior trades the mean is also 0, so the fallback denominator
// collapses to a small epsilon and ANY nonzero trade size reads as
// maximally large relative to "no history at all" -- a deliberate,
// documented consequence of the formula, not a hidden special case.
double computeSizeScore(const WalletHistoryRecord& priorHistory, double tradeSize, double zScoreCap) {
    double denom;
    if (priorHistory.trade_count >= 2 && priorHistory.sizeStddev() > 1e-9) {
        denom = priorHistory.sizeStddev();
    } else {
        denom = std::max(priorHistory.size_mean * 0.1, 1e-6);
    }
    double z = (tradeSize - priorHistory.size_mean) / denom;
    // Only unusually LARGE trades are suspicious here; a smaller-than-usual
    // trade (negative z) is not, so this is one-sided.
    return clamp01(z / zScoreCap);
}

// Time since the wallet's first known trade. A wallet with no prior trades
// at all (first_seen_unix == 0) is, by definition, brand new -- age 0,
// maximal age_score -- not a missing-data case needing a guess.
double computeAgeScore(const WalletHistoryRecord& priorHistory, uint64_t nowUnix, double ageDaysCap) {
    if (priorHistory.first_seen_unix == 0) return 1.0;
    if (nowUnix <= priorHistory.first_seen_unix) return 1.0;  // clock skew guard, treat as brand new
    double ageDays = static_cast<double>(nowUnix - priorHistory.first_seen_unix) / 86400.0;
    return clamp01(1.0 - ageDays / ageDaysCap);
}

// Herfindahl-Hirschman Index (sum of squared category shares) over the
// wallet's category history INCLUDING this trade's own category -- a
// standard, simple concentration measure: en.wikipedia.org/wiki/
// Herfindahl%E2%80%93Hirschman_Index. Ranges from ~0 (spread evenly across
// many categories) to 1 (all trades, including this one, in a single
// category). This trade is included (unlike size/age above) because
// concentration is a property of the aggregate distribution as it stands
// right now, not a "new data point vs. prior baseline" comparison -- and
// including it means a wallet's very first trade always yields a
// well-defined HHI of 1.0 (100% of its one trade so far in one category)
// rather than needing an arbitrary zero-data special case.
double computeConcentrationScore(const WalletHistoryRecord& priorHistory, const std::string& tradeCategory) {
    std::map<std::string, uint64_t> counts = priorHistory.category_counts;
    ++counts[tradeCategory];

    uint64_t total = priorHistory.trade_count + 1;
    double sumSquaredShares = 0.0;
    for (const auto& kv : counts) {
        double share = static_cast<double>(kv.second) / static_cast<double>(total);
        sumSquaredShares += share * share;
    }
    return clamp01(sumSquaredShares);
}

} // namespace

AnomalyScore computeAnomalyScore(const WalletHistoryRecord& priorHistory, double tradeSize,
                                  const std::string& tradeCategory, uint64_t nowUnix, const AnomalyScoreConfig& cfg) {
    AnomalyScore score;
    score.size_score = computeSizeScore(priorHistory, tradeSize, cfg.sizeZScoreCap);
    score.age_score = computeAgeScore(priorHistory, nowUnix, cfg.ageDaysCap);
    score.concentration_score = computeConcentrationScore(priorHistory, tradeCategory);

    // Simple, documented combination: equally-weighted average of the
    // three components. Not validated against real labeled data -- a
    // starting point for tuning, deliberately simple rather than a
    // weighted/learned combination, per the same "simple and inspectable
    // over black box" philosophy as the components themselves.
    score.total_score = (score.size_score + score.age_score + score.concentration_score) / 3.0;
    score.flagged = score.total_score >= cfg.flagThreshold;
    score.computed = true;
    score.note = "scored against " + std::to_string(priorHistory.trade_count) + " prior trade(s), category='" +
                 tradeCategory + "'";
    return score;
}
