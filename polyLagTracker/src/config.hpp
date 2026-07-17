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
    std::vector<std::string> asset_ids;          // required unless dump_raw_messages is set
    int reconnect_min_backoff_ms = 500;
    int reconnect_max_backoff_ms = 30000;
    int ping_interval_sec = 30;

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
