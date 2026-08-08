#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class MatchMode {
    Auto,     // tx hash if present in payload, else fuzzy (if configured)
    TxHash,   // only ever match via tx hash; unmatched if payload lacks one
    Fuzzy,    // only ever attempt the fuzzy log-scan path
    Off       // don't attempt any on-chain resolution (WS-only capture)
};

struct AppConfig {
    // --- WebSocket ---
    std::string ws_url = "wss://ws-subscriptions-clob.polymarket.com/ws/market";
    std::vector<std::string> asset_ids;          // required unless auto_discover_assets is set
    int reconnect_min_backoff_ms = 500;
    int reconnect_max_backoff_ms = 30000;
    int ping_interval_sec = 30;

    // --- Auto-discovery of active markets ---
    // Polymarket's highest-volume markets (e.g. the BTC/ETH/XRP 5-minute
    // up/down markets) churn and get replaced every few minutes, so a
    // hand-curated --asset-ids list goes stale fast. When set, asset_ids is
    // populated at startup (and periodically refreshed) from a sample of
    // recent trades instead of being supplied directly. If asset_ids is
    // also non-empty, asset_ids wins and this is ignored (see loadConfig).
    bool auto_discover_assets = false;
    int discover_trade_sample_size = 500;    // trades sampled per discovery call
    int discover_top_n = 40;                 // how many of the most-frequent assets to subscribe to
    int discover_refresh_interval_sec = 1800; // 0 disables periodic refresh (startup discovery only)

    // --- Category-based discovery of markets (alternative to the above) ---
    // See README "Category-based market discovery". Mutually exclusive
    // with auto_discover_assets (checked in loadConfig) -- volume-based
    // discovery picks the highest-*trade-volume* markets, which structurally
    // favors short-duration speculative markets (crypto up/down contracts)
    // that have no possibility of genuine insider information; this mode
    // instead subscribes to EVERY currently open market under a configured
    // set of Gamma API tag slugs, regardless of volume, since markets
    // resolved by real-world political/geopolitical/regulatory decisions
    // are where informed trading is structurally possible -- and those are
    // exactly the low-volume markets volume-based discovery would never
    // pick up (confirmed by the Stage 1 sanity check: volume-based
    // discovery left low-frequency, human-scale wallets at just 2.4% of
    // the watched population).
    bool discover_by_category = false;
    // Confirmed live against the real /tags taxonomy (gamma-api.polymarket.com
    // GET /events?tag_slug=<slug>&closed=false), not assumed -- see README
    // for the verification. Covers politics, geopolitics, military,
    // elections, and regulatory/monetary-policy categories.
    std::vector<std::string> discover_tags = {
        "politics", "elections", "geopolitics", "military", "military-strikes", "nato",
        "tariffs", "supreme-court", "congress", "fomc", "interest-rates", "monetary-policy",
        "government-shutdown", "regulation", "fda", "sec",
    };

    // --- Polygon RPC ---
    std::string rpc_url;                          // required (e.g. Alchemy/Infura/public RPC)
    long rpc_timeout_ms = 8000;
    int rpc_poll_max_attempts = 10;                // for pending-tx retry
    int rpc_poll_interval_ms = 3000;

    // --- Trade -> on-chain matching ---
    MatchMode match_mode = MatchMode::TxHash;
    // Fuzzy log-scan is opt-in and requires the two fields below to be filled
    // in by hand once you've confirmed them against Polygonscan/Polymarket's
    // own contract docs. This tool deliberately ships with no hardcoded
    // contract address or event topic hash for the fuzzy path -- guessing
    // wrong here would silently produce wrong lag numbers with no error.
    std::string exchange_contract_address;         // e.g. Polymarket CTF Exchange on Polygon
    std::string order_filled_topic0;                // keccak256("OrderFilled(...)") topic hash
    uint64_t fuzzy_block_window = 50;               // blocks scanned each side of the estimate
    double fuzzy_price_tolerance_pct = 0.5;         // 0.5 = 0.5%
    double fuzzy_size_tolerance_pct = 0.5;
    double polygon_avg_block_time_sec = 2.0;        // used to estimate a block range from wall time

    // --- Storage ---
    std::string output_path = "poly_lag.csv";
    std::string log_file;                           // optional, empty = stdout/stderr only
    int log_interval_sec = 300;

    // --- Concurrency ---
    int worker_threads = 4;
    int queue_max_size = 5000;

    // --- Safety ---
    bool force_unsynced_clock = false;

    // --- Wallet resolution ---
    // No config needed: wallet addresses are now decoded directly from the
    // CTF Exchange V2 contract's OrderFilled event logs in the same
    // eth_getTransactionReceipt call already made for on-chain confirmation
    // above -- see wallet_resolve.hpp/cpp for how and why (validated
    // against 83/83 known-correct wallets, replacing an earlier Data-API
    // lookup that only resolved ~18% of trades due to that endpoint's
    // unpredictable indexing lag). Always on whenever a receipt is
    // available; nothing to opt into or tune.

    // --- Wallet history + anomaly scoring (Stage 1) ---
    // See README "Wallet history store" and "Anomaly scoring" -- always on
    // whenever wallet resolution succeeds for a trade; a wallet not yet
    // cached is backfilled once via the Data API's confirmed-live
    // GET /trades?user=<addr> endpoint (wallet_history_fetch.hpp), then
    // updated incrementally in the local SQLite cache thereafter.
    std::string wallet_history_db_path = "wallet_history.db";
    // A trade's combined anomaly score (equally-weighted average of the
    // size/age/concentration components, each in [0,1] -- see
    // anomaly_score.hpp) at or above this is marked flagged in the CSV and
    // logged distinctly, and hands off to the Stage 2 hook (see
    // stage2_hook.hpp). 0.7 is a starting point for tuning against real
    // data, not a validated threshold.
    double anomaly_score_flag_threshold = 0.7;

    // --- Wallet frequency segmentation ---
    // See README "Wallet frequency segmentation" -- classifies each wallet
    // into low/medium/high frequency BEFORE anomaly scoring, using its
    // cached trade_count and trades/day since first seen. Only Low-tier
    // wallets get the existing three-component score (anomaly_score.hpp);
    // Medium and High are recorded with anomaly_scope=excluded_*_frequency
    // rather than scored -- see wallet_segment.hpp for why. Defaults below
    // were confirmed against a live run's tier distribution, not picked
    // arbitrarily; tune if your own run's distribution looks degenerate
    // (e.g. everything landing in one tier).
    uint64_t segment_low_max_trades = 50;
    double segment_low_max_trades_per_day = 5.0;
    uint64_t segment_high_min_trades = 2000;
    double segment_high_min_trades_per_day = 50.0;

    // --- Raw payload inspection mode ---
    // If > 0, connects, subscribes, writes this many raw WS payloads to
    // dump_raw_path, then exits. No RPC calls, no storage writes. Use this
    // first on a freshly deployed server to confirm the real message
    // schema (does it include a tx hash? what are the field names?) before
    // deciding --match-mode and trusting any lag numbers.
    int dump_raw_messages = 0;
    std::string dump_raw_path = "raw_messages.jsonl";
};

// Loads defaults, then a config file (if --config given), then CLI args, in
// that order (later sources override earlier ones). Returns false (and
// prints usage/errors) if the args are invalid or --help was requested.
bool loadConfig(int argc, char** argv, AppConfig* cfg);

std::string matchModeToString(MatchMode m);
