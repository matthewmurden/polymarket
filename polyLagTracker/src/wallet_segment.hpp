#pragma once

#include <cstdint>
#include <string>

#include "wallet_history_store.hpp"

// Wallet frequency segmentation -- computed BEFORE any anomaly scoring, and
// used to decide whether a wallet is even in scope for the existing
// three-component scorer (anomaly_score.hpp) at all.
//
// Why this exists: a manual sanity check of the unsegmented scorer found
// that age_score and concentration_score were both compressed near their
// ceiling for the majority of scored trades, not because most wallets are
// genuinely new/narrowly-focused, but because the population being scored
// mixes two structurally different kinds of wallet:
//   - low-frequency, human-scale wallets (a handful of trades) -- the shape
//     of the known real insider-trading cases in this project's own FFIC
//     reference data: a small number of unusually large, well-timed trades,
//     not sustained activity.
//   - high-frequency, likely-automated/market-maker wallets (thousands of
//     trades), which routinely hit wallet_history_fetch.hpp's `maxPages`
//     backfill cap. Hitting that cap makes an extremely established,
//     obviously-not-new wallet look artificially "new" to age_score (its
//     cached first_seen_unix is just "however far back the capped backfill
//     reached", not the wallet's true first trade) -- the exact opposite of
//     what age_score is meant to signal.
//
// This does NOT attempt to fix age_score/concentration_score's formulas
// for high-frequency wallets -- see anomaly_score.hpp, unchanged. Instead,
// scoring is restructured around which population a wallet belongs to: see
// main.cpp's scoreTradeStage1 for how each tier is routed.
enum class WalletFrequencyTier {
    Low,
    Medium,
    High,
};

std::string tierToString(WalletFrequencyTier tier);

struct WalletSegmentConfig {
    // A wallet is Low-frequency if its cached trade_count is at or below
    // this AND its trades/day (see classifyWallet) is at or below
    // lowMaxTradesPerDay. Defaults are starting points confirmed against a
    // live run (see README "Wallet frequency segmentation"), not
    // arbitrary -- tune via the CLI flags below if real data suggests
    // otherwise.
    uint64_t lowMaxTrades = 50;
    double lowMaxTradesPerDay = 5.0;

    // A wallet is High-frequency if its cached trade_count is at or above
    // this OR its trades/day is at or above highMinTradesPerDay. A wallet
    // that already hit wallet_history_fetch.hpp's backfill cap will always
    // satisfy the trade_count condition here, so it's always High
    // regardless of the (potentially cap-foreshortened, but only ever
    // biased toward looking MORE frequent, not less -- see
    // wallet_segment.cpp) trades/day estimate.
    uint64_t highMinTrades = 2000;
    double highMinTradesPerDay = 50.0;

    // Everything that's neither Low nor High is Medium. Medium-frequency
    // wallets are, for now, also excluded from the existing scorer (see
    // main.cpp) rather than scored -- the existing formulas were only
    // confirmed meaningful for the Low population specifically (see
    // anomaly_score.hpp's doc comment and the README), and Medium wasn't
    // covered by that confirmation either. A dedicated approach for Medium
    // is a candidate for future work, not attempted here.
};

// Classifies `history` (the wallet's cached history, from BEFORE the
// current trade is folded in -- same "prior history" convention as
// computeAnomalyScore) into a frequency tier. `nowUnix` is wall-clock time
// at classification time, used the same way anomaly_score.hpp uses it for
// age_score.
WalletFrequencyTier classifyWallet(const WalletHistoryRecord& priorHistory, uint64_t nowUnix,
                                    const WalletSegmentConfig& cfg);
