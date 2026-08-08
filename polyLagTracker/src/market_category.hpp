#pragma once

#include <string>

#include "wallet_history_store.hpp"

// Resolves a market's category/tag label for concentration scoring (see
// anomaly_score.hpp) via Polymarket's Gamma API. This is NOT available on
// the Data API's /trades response used elsewhere in this tool -- confirmed
// live that trade objects carry no category/tag field at all.
//
// Two-hop lookup, confirmed live against real markets: conditionId ->
// GET /markets?condition_ids=<id> -> embedded events[0].id -> GET
// /events?id=<id> -> tags[]. Of the tags returned, the one with the lowest
// numeric "id" is used as the "primary" category -- confirmed empirically
// (not guessed) that Polymarket's broadest, most-foundational category
// tags (e.g. "Sports") consistently have the lowest ids, having been
// created earliest in the tag taxonomy. This is a documented heuristic,
// not a guarantee of the API's contract, and is a reasonable first place
// to look if categorization looks wrong during tuning.
//
// Also confirmed live: this lookup reliably succeeds for currently-active
// markets, but returns nothing for markets that have already resolved/
// expired and been pruned from Gamma's active index (observed directly
// with an already-resolved 5-minute BTC up/down market). Since this is
// called near-real-time as trades arrive, that's rarely hit in practice --
// but when it is, this returns the sentinel string "unknown" rather than
// failing. That's a legitimate, cacheable outcome, not an error.
//
// Results are cached forever in `store` (a market's category doesn't
// change) so the network round trip(s) happen at most once per market for
// the whole run, not once per wallet per trade referencing it.
//
// IMPORTANT, confirmed live while load-testing this: call this (the
// network-fetching version) only for a wallet's current, just-arrived
// trade, where the market is virtually certain to still be active. Do NOT
// call this per-trade during a wallet's historical backfill
// (wallet_history_fetch.cpp) -- a live test hitting it for every
// historical trade in a backfill caused the vast majority of lookups to
// fail (most of a wallet's PAST markets have already expired/been pruned
// by the time it's backfilled) while still paying the full network
// round-trip cost for each failure, which backed up the whole worker
// pool's trade queue badly. Use getCachedCategoryOnly() below for
// historical trades instead.
std::string resolveMarketCategory(const std::string& conditionId, WalletHistoryStore* store, long timeoutMs);

// Cache-only lookup, no network call: returns the cached category if one
// exists, or the sentinel "unknown" immediately if not (without caching
// that "unknown" -- an uncached market may still resolve correctly later
// via resolveMarketCategory() for a live trade referencing it). Use this
// for historical trades during a wallet's backfill, where most markets
// have already expired and a network round trip per trade is wasted cost
// at scale -- see the load-testing note above.
std::string getCachedCategoryOnly(const std::string& conditionId, WalletHistoryStore* store);
