#pragma once

#include <cstdint>
#include <string>

#include "wallet_history_store.hpp"

// Simple, fully-documented per-trade anomaly score -- Stage 1 of a planned
// two-stage detection system. This is fast, wallet-level scoring only,
// computed entirely from data already cached locally (see
// wallet_history_store.hpp); no new network call happens here. Expensive
// funding-graph tracing is Stage 2, deliberately out of scope for this
// component -- see stage2_hook.hpp for where a wallet crossing the flag
// threshold hands off to it.
//
// Every component score is a plain double in [0, 1] (0 = unremarkable by
// that one measure, 1 = maximally suspicious by that one measure), each
// computed by a simple, separately-documented formula below -- not a
// trained model or black box, since this is meant to be read, understood,
// and hand-tuned as real data comes in.
struct AnomalyScore {
    bool computed = false;             // false if scoring wasn't attempted at all -- see `scope`/`note`
    double size_score = 0.0;           // how large this trade is vs. this wallet's own prior typical size
    double age_score = 0.0;            // how new this wallet is (time since its first known trade)
    double concentration_score = 0.0;  // how concentrated this wallet's trading (incl. this trade) is in one category
    double total_score = 0.0;          // combined score, see computeAnomalyScore()
    bool flagged = false;              // total_score >= the configured threshold
    std::string note;                  // why unscored, or a short diagnostic (e.g. prior trade count used)

    // Set by main.cpp's scoreTradeStage1, not by computeAnomalyScore()
    // itself (this struct is also used for wallets this scorer never runs
    // on at all -- see wallet_segment.hpp). `tier` is the wallet's
    // classified frequency tier ("low"/"medium"/"high"), empty if
    // classification wasn't reached (e.g. wallet unresolved). `scope` is
    // one of: "scored" (Low tier, computed=true), "excluded_medium_frequency",
    // "excluded_high_frequency", "unscored" (history unavailable), or
    // "wallet_unresolved".
    std::string tier;
    std::string scope;
};

struct AnomalyScoreConfig {
    // Trade-size z-score (in standard deviations above the wallet's prior
    // mean) that maps to a maximal (1.0) size_score. 4 sigma is
    // deliberately generous -- a starting point for tuning against real
    // data, not a validated threshold.
    double sizeZScoreCap = 4.0;

    // Wallet age, in days since its first known trade, at or beyond which
    // age_score bottoms out at 0. A wallet younger than this contributes a
    // proportionally higher age_score. 30 days is a starting point, not a
    // validated threshold.
    double ageDaysCap = 30.0;

    // Trade above the flag threshold gets flagged; see main.cpp
    // --anomaly-score-flag-threshold.
    double flagThreshold = 0.7;
};

// Computes all component scores for one trade against `priorHistory` --
// the wallet's cached history from BEFORE this trade (i.e. call this
// before folding the current trade into the cached record via
// foldTradeIntoHistory(), not after: comparing a trade against a baseline
// that already includes itself would dilute exactly the signal being
// measured, especially for low-trade-count wallets where a single new
// data point can dominate the mean/stddev it would be compared against).
//
// `tradeSize`/`tradeCategory` are this trade's own values; `nowUnix` is
// wall-clock time at scoring time (not necessarily the trade's own
// reported timestamp, in case of WS/RPC lag).
AnomalyScore computeAnomalyScore(const WalletHistoryRecord& priorHistory, double tradeSize,
                                  const std::string& tradeCategory, uint64_t nowUnix, const AnomalyScoreConfig& cfg);
