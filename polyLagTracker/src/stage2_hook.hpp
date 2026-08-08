#pragma once

#include <string>

#include "anomaly_score.hpp"

// Stage 2 integration point -- SCAFFOLD ONLY, deliberately not implemented
// here.
//
// This tool's detection design is two-stage: Stage 1 (everything else in
// wallet_history_store.hpp/wallet_history_fetch.hpp/market_category.hpp/
// anomaly_score.hpp) is fast, cheap, wallet-level scoring run on every
// single trade. Stage 2 -- tracing a flagged wallet's funding graph
// (where its collateral came from, whether it connects to other flagged
// or related wallets, etc.) -- is expensive by comparison and is meant to
// run only for the small subset of wallets Stage 1 actually flags, not on
// every trade. That tracing logic does not exist yet and is explicitly
// out of scope for this change.
//
// onWalletFlagged() is the hand-off point: called exactly once per trade
// that crosses the configured anomaly-score threshold (see
// main.cpp/config.hpp --anomaly-score-flag-threshold), after that trade's
// row has already been scored and written to CSV -- Stage 1's own output
// is never blocked or delayed waiting on this. A future Stage 2
// implementation should replace this function's body (queue the wallet
// for async funding-graph tracing, write to a separate output/queue,
// etc.) without needing to change anything upstream of this call site.
void onWalletFlagged(const std::string& walletAddress, const AnomalyScore& score);
