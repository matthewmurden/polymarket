#pragma once

#include <atomic>
#include <string>

#include "wallet_history_store.hpp"

struct WalletHistoryFetchConfig {
    int pageSize = 500;
    int maxPages = 20;   // safety cap: 20*500 = 10,000 trades backfilled per wallet max, a bound not an unbounded fetch
    long timeoutMs = 8000;
};

// One-time backfill for a wallet this tool has never seen before: pages
// through the Data API's confirmed-live GET /trades?user=<addr> endpoint
// (NOT /trades?wallet=, which silently returns unrelated unfiltered trades
// instead of erroring or filtering -- confirmed live, see README "Wallet
// history store"), folding every historical trade into a fresh
// WalletHistoryRecord via foldTradeIntoHistory(), resolving each trade's
// market category along the way (see market_category.hpp). Does not read
// or write the cache itself -- the caller decides when to upsert.
//
// Two validation guards exist specifically because of a real gotcha
// confirmed live: the endpoint silently ignores a malformed `user` value
// and falls back to returning generic, unfiltered, unrelated trades rather
// than an error or an empty array. So this (a) rejects `walletAddress`
// up front unless it's exactly "0x" + 40 hex chars, and (b) independently
// checks every returned trade's own proxyWallet actually matches the
// query, discarding (and logging) any page where it doesn't rather than
// trusting it. Skipping either check would risk silently attributing a
// random wallet's trading history to the wrong address in the cache.
//
// A wallet that genuinely has zero prior trades on its very first page is
// a real, meaningful, and correctly-cached result:
// had_zero_history_on_first_lookup = true, not an error -- see Part 2 of
// the design this implements.
WalletHistoryRecord fetchWalletHistoryBackfill(const std::string& walletAddress, WalletHistoryStore* store,
                                                const WalletHistoryFetchConfig& cfg,
                                                const std::atomic<bool>* stopRequested);
