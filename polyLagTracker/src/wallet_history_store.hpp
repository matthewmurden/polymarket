#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

// Forward-declared to avoid leaking <sqlite3.h> into every includer.
struct sqlite3;

// One wallet's trading history as known so far, built up incrementally --
// once from an initial backfill via the Data API's /trades?user=<addr>
// endpoint (see wallet_history_fetch.hpp) when a wallet is first seen, then
// folded forward one trade at a time as this tool observes more trades from
// it live over WS. Whichever of "backfill" or "live update" happens for a
// given trade, both go through foldTradeIntoHistory() below so the record
// ends up in an identical shape either way.
//
// Trade size mean/variance are tracked via Welford's online algorithm
// (size_mean, size_m2) instead of storing every individual trade size --
// O(1) space per wallet, a standard, numerically-stable incremental
// formula (not a bespoke one): see
// https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
struct WalletHistoryRecord {
    std::string wallet_address;
    bool known = false;  // false: no cache row exists yet, this wallet has never been looked up at all

    // True specifically when the *initial* Data API backfill found zero
    // prior trades for this wallet -- i.e. a real "brand new wallet" signal,
    // distinct from `known == false` (never looked up) and distinct from a
    // failed lookup (see wallet_history_fetch.hpp for how those are told
    // apart). This is a meaningful case, not an error: see Part 2 of the
    // design this implements.
    bool had_zero_history_on_first_lookup = false;

    uint64_t trade_count = 0;      // total trades known for this wallet (backfilled + live)
    uint64_t first_seen_unix = 0;  // earliest trade timestamp seen; 0 if trade_count == 0. Account-age proxy.
    double size_mean = 0.0;        // Welford running mean of trade size
    double size_m2 = 0.0;          // Welford running sum of squared deviations from the mean

    // category/tag label -> number of trades seen in a market carrying
    // that category. The *set* of keys is "the set of market categories
    // this wallet has traded in"; the counts drive the concentration score
    // (see anomaly_score.hpp). "unknown" is a legitimate category value
    // for markets whose category couldn't be looked up (see
    // market_category.hpp) -- not an error case to special-case here.
    std::map<std::string, uint64_t> category_counts;

    uint64_t last_updated_unix = 0;

    // Sample variance (Bessel-corrected); 0 with fewer than 2 trades.
    double sizeVariance() const;
    double sizeStddev() const;
};

// Folds one more trade (its size, resolved category, and trade-time unix
// timestamp) into `rec` in place: bumps trade_count, lowers first_seen_unix
// if this trade is older than anything seen so far, updates the Welford
// mean/M2 for size, and increments category_counts[category]. Called once
// per historical trade during the initial backfill, and once per trade for
// every subsequent live update -- the single shared code path that keeps
// both routes producing identically-shaped records.
void foldTradeIntoHistory(WalletHistoryRecord* rec, double size, const std::string& category, uint64_t tradeUnix);

// SQLite-backed cache backing two things:
//   1. one row per wallet (WalletHistoryRecord above)
//   2. a small conditionId -> category cache (see market_category.hpp), so
//      each market's category/tag lookup happens at most once ever across
//      the whole run, not once per wallet per trade referencing it.
//
// A single sqlite3 connection guarded by one mutex -- not a connection
// pool, not WAL-mode tuning. This tool's trade volume doesn't need either,
// and one lock is trivial to reason about; see README "Wallet history
// store" for why this was chosen over a heavier design.
class WalletHistoryStore {
public:
    explicit WalletHistoryStore(const std::string& dbPath);
    ~WalletHistoryStore();

    WalletHistoryStore(const WalletHistoryStore&) = delete;
    WalletHistoryStore& operator=(const WalletHistoryStore&) = delete;

    // Local read only, no network call. Returns a record with known=false
    // if this wallet has never been cached (i.e. a genuine cache miss that
    // the caller should resolve via wallet_history_fetch.hpp).
    WalletHistoryRecord get(const std::string& walletAddress);

    // Insert-or-replace the full record for this wallet.
    void upsert(const WalletHistoryRecord& rec);

    // Market category cache. nullopt means "never looked up" (a genuine
    // cache miss); the cached value itself may legitimately be the
    // sentinel string "unknown" (see market_category.cpp) when a lookup
    // was attempted but no tag data was available -- both are meaningful,
    // distinct outcomes, neither is an error.
    std::optional<std::string> getCachedCategory(const std::string& conditionId);
    void cacheCategory(const std::string& conditionId, const std::string& category);

private:
    sqlite3* db_ = nullptr;
    std::mutex mtx_;
};
