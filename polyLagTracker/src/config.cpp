#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::string t = trim(item);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

bool parseMatchMode(const std::string& s, MatchMode* out) {
    if (s == "auto") { *out = MatchMode::Auto; return true; }
    if (s == "txhash") { *out = MatchMode::TxHash; return true; }
    if (s == "fuzzy") { *out = MatchMode::Fuzzy; return true; }
    if (s == "off") { *out = MatchMode::Off; return true; }
    return false;
}

void printUsage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "  --config PATH                   key=value config file (CLI flags below override it)\n"
        "\n"
        "  --ws-url URL                     Polymarket CLOB market WS URL\n"
        "  --asset-ids ID1,ID2,...          token/asset ids to subscribe to (required unless --auto-discover-assets)\n"
        "  --reconnect-min-backoff-ms N     IXWebSocket min retry wait (default 500)\n"
        "  --reconnect-max-backoff-ms N     IXWebSocket max retry wait (default 30000)\n"
        "  --ping-interval-sec N            WS keepalive ping interval (default 30)\n"
        "\n"
        "  --auto-discover-assets           pick the subscription set from currently active trades instead\n"
        "                                   of a fixed --asset-ids list (ignored if --asset-ids is also set)\n"
        "  --discover-trade-sample-size N   trades sampled per discovery call (default 500)\n"
        "  --discover-top-n N               how many of the most-frequent assets to subscribe to (default 40)\n"
        "  --discover-refresh-interval-sec N  re-run discovery this often, 0 disables refresh (default 1800;\n"
        "                                   auto-bumped to 14400 for --discover-by-category unless set explicitly)\n"
        "\n"
        "  --discover-by-category          subscribe to every OPEN market under --discover-tags, regardless\n"
        "                                   of trade volume, instead of --auto-discover-assets' top-N-by-volume\n"
        "                                   (mutually exclusive with --auto-discover-assets; see README\n"
        "                                   \"Category-based market discovery\")\n"
        "  --discover-tags T1,T2,...        Gamma API tag slugs to discover under (default: a curated\n"
        "                                   politics/geopolitics/military/elections/regulatory set,\n"
        "                                   confirmed live -- see README)\n"
        "\n"
        "  --rpc-url URL                    Polygon JSON-RPC endpoint (required unless --match-mode off)\n"
        "  --rpc-timeout-ms N               per-call RPC timeout (default 8000)\n"
        "  --rpc-poll-max-attempts N        retries while a tx is unconfirmed (default 10)\n"
        "  --rpc-poll-interval-ms N         delay between confirmation polls (default 3000)\n"
        "\n"
        "  --match-mode auto|txhash|fuzzy|off   how to cross-reference trades (default txhash)\n"
        "  --exchange-contract ADDR         contract address for fuzzy log-scan (no default; you must supply this)\n"
        "  --order-filled-topic0 HEX        keccak256 event topic for fuzzy log-scan (no default; you must supply this)\n"
        "  --fuzzy-block-window N           blocks scanned each side of the time estimate (default 50)\n"
        "  --fuzzy-price-tolerance-pct F    default 0.5\n"
        "  --fuzzy-size-tolerance-pct F     default 0.5\n"
        "  --polygon-block-time-sec F       used to estimate a block range from wall time (default 2.0)\n"
        "\n"
        "  --output PATH                    CSV output path (default poly_lag.csv)\n"
        "  --log-file PATH                  optional log file (default: stdout/stderr only)\n"
        "  --log-interval-sec N             periodic stats log interval (default 300)\n"
        "\n"
        "  --worker-threads N               RPC-resolution worker threads (default 4)\n"
        "  --queue-max-size N               bounded trade queue size (default 5000)\n"
        "\n"
        "  --force-unsynced-clock           proceed even if system clock isn't NTP-synced (NOT recommended)\n"
        "  --dump-raw-messages N            write N raw WS payloads to --dump-raw-path, then exit\n"
        "  --dump-raw-path PATH             default raw_messages.jsonl\n"
        "\n"
        "  (wallet address resolution is always on and needs no configuration --\n"
        "   it's decoded from the same on-chain receipt used for tx confirmation,\n"
        "   see README \"Wallet address resolution\")\n"
        "\n"
        "  --wallet-history-db PATH        SQLite cache for wallet history/anomaly scoring (default wallet_history.db)\n"
        "  --anomaly-score-flag-threshold F  combined score at/above which a trade is flagged (default 0.7)\n"
        "\n"
        "  --segment-low-max-trades N        Low tier: trade_count ceiling (default 50)\n"
        "  --segment-low-max-trades-per-day F  Low tier: trades/day ceiling (default 5.0)\n"
        "  --segment-high-min-trades N       High tier: trade_count floor (default 2000)\n"
        "  --segment-high-min-trades-per-day F High tier: trades/day floor (default 50.0)\n"
        "  --help                           show this message\n",
        prog);
}

bool loadConfigFile(const std::string& path, AppConfig* cfg) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "could not open config file: %s\n", path.c_str());
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (key == "ws_url") cfg->ws_url = val;
        else if (key == "asset_ids") cfg->asset_ids = splitCsv(val);
        else if (key == "reconnect_min_backoff_ms") cfg->reconnect_min_backoff_ms = std::stoi(val);
        else if (key == "reconnect_max_backoff_ms") cfg->reconnect_max_backoff_ms = std::stoi(val);
        else if (key == "ping_interval_sec") cfg->ping_interval_sec = std::stoi(val);
        else if (key == "auto_discover_assets") cfg->auto_discover_assets = (val == "true" || val == "1");
        else if (key == "discover_trade_sample_size") cfg->discover_trade_sample_size = std::stoi(val);
        else if (key == "discover_top_n") cfg->discover_top_n = std::stoi(val);
        else if (key == "discover_refresh_interval_sec") cfg->discover_refresh_interval_sec = std::stoi(val);
        else if (key == "discover_by_category") cfg->discover_by_category = (val == "true" || val == "1");
        else if (key == "discover_tags") cfg->discover_tags = splitCsv(val);
        else if (key == "rpc_url") cfg->rpc_url = val;
        else if (key == "rpc_timeout_ms") cfg->rpc_timeout_ms = std::stol(val);
        else if (key == "rpc_poll_max_attempts") cfg->rpc_poll_max_attempts = std::stoi(val);
        else if (key == "rpc_poll_interval_ms") cfg->rpc_poll_interval_ms = std::stoi(val);
        else if (key == "match_mode") {
            if (!parseMatchMode(val, &cfg->match_mode)) {
                std::fprintf(stderr, "invalid match_mode in config: %s\n", val.c_str());
                return false;
            }
        }
        else if (key == "exchange_contract_address") cfg->exchange_contract_address = val;
        else if (key == "order_filled_topic0") cfg->order_filled_topic0 = val;
        else if (key == "fuzzy_block_window") cfg->fuzzy_block_window = std::stoull(val);
        else if (key == "fuzzy_price_tolerance_pct") cfg->fuzzy_price_tolerance_pct = std::stod(val);
        else if (key == "fuzzy_size_tolerance_pct") cfg->fuzzy_size_tolerance_pct = std::stod(val);
        else if (key == "polygon_avg_block_time_sec") cfg->polygon_avg_block_time_sec = std::stod(val);
        else if (key == "output_path") cfg->output_path = val;
        else if (key == "log_file") cfg->log_file = val;
        else if (key == "log_interval_sec") cfg->log_interval_sec = std::stoi(val);
        else if (key == "worker_threads") cfg->worker_threads = std::stoi(val);
        else if (key == "queue_max_size") cfg->queue_max_size = std::stoi(val);
        else if (key == "force_unsynced_clock") cfg->force_unsynced_clock = (val == "true" || val == "1");
        else if (key == "dump_raw_messages") cfg->dump_raw_messages = std::stoi(val);
        else if (key == "dump_raw_path") cfg->dump_raw_path = val;
        else if (key == "wallet_history_db_path") cfg->wallet_history_db_path = val;
        else if (key == "anomaly_score_flag_threshold") cfg->anomaly_score_flag_threshold = std::stod(val);
        else if (key == "segment_low_max_trades") cfg->segment_low_max_trades = std::stoull(val);
        else if (key == "segment_low_max_trades_per_day") cfg->segment_low_max_trades_per_day = std::stod(val);
        else if (key == "segment_high_min_trades") cfg->segment_high_min_trades = std::stoull(val);
        else if (key == "segment_high_min_trades_per_day") cfg->segment_high_min_trades_per_day = std::stod(val);
        else std::fprintf(stderr, "warning: unknown config key '%s' ignored\n", key.c_str());
    }
    return true;
}

} // namespace

std::string matchModeToString(MatchMode m) {
    switch (m) {
        case MatchMode::Auto: return "auto";
        case MatchMode::TxHash: return "txhash";
        case MatchMode::Fuzzy: return "fuzzy";
        case MatchMode::Off: return "off";
    }
    return "unknown";
}

bool loadConfig(int argc, char** argv, AppConfig* cfg) {
    *cfg = AppConfig{};

    // First pass: just look for --config so file values load before CLI
    // overrides are applied below.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            if (!loadConfigFile(argv[i + 1], cfg)) return false;
            break;
        }
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--config") { next("--config"); } // already handled above
        else if (arg == "--ws-url") cfg->ws_url = next("--ws-url");
        else if (arg == "--asset-ids") cfg->asset_ids = splitCsv(next("--asset-ids"));
        else if (arg == "--reconnect-min-backoff-ms") cfg->reconnect_min_backoff_ms = std::stoi(next(arg.c_str()));
        else if (arg == "--reconnect-max-backoff-ms") cfg->reconnect_max_backoff_ms = std::stoi(next(arg.c_str()));
        else if (arg == "--ping-interval-sec") cfg->ping_interval_sec = std::stoi(next(arg.c_str()));
        else if (arg == "--auto-discover-assets") cfg->auto_discover_assets = true;
        else if (arg == "--discover-trade-sample-size") cfg->discover_trade_sample_size = std::stoi(next(arg.c_str()));
        else if (arg == "--discover-top-n") cfg->discover_top_n = std::stoi(next(arg.c_str()));
        else if (arg == "--discover-refresh-interval-sec") cfg->discover_refresh_interval_sec = std::stoi(next(arg.c_str()));
        else if (arg == "--discover-by-category") cfg->discover_by_category = true;
        else if (arg == "--discover-tags") cfg->discover_tags = splitCsv(next(arg.c_str()));
        else if (arg == "--rpc-url") cfg->rpc_url = next("--rpc-url");
        else if (arg == "--rpc-timeout-ms") cfg->rpc_timeout_ms = std::stol(next(arg.c_str()));
        else if (arg == "--rpc-poll-max-attempts") cfg->rpc_poll_max_attempts = std::stoi(next(arg.c_str()));
        else if (arg == "--rpc-poll-interval-ms") cfg->rpc_poll_interval_ms = std::stoi(next(arg.c_str()));
        else if (arg == "--match-mode") {
            std::string v = next("--match-mode");
            if (!parseMatchMode(v, &cfg->match_mode)) {
                std::fprintf(stderr, "invalid --match-mode: %s (want auto|txhash|fuzzy|off)\n", v.c_str());
                return false;
            }
        }
        else if (arg == "--exchange-contract") cfg->exchange_contract_address = next(arg.c_str());
        else if (arg == "--order-filled-topic0") cfg->order_filled_topic0 = next(arg.c_str());
        else if (arg == "--fuzzy-block-window") cfg->fuzzy_block_window = std::stoull(next(arg.c_str()));
        else if (arg == "--fuzzy-price-tolerance-pct") cfg->fuzzy_price_tolerance_pct = std::stod(next(arg.c_str()));
        else if (arg == "--fuzzy-size-tolerance-pct") cfg->fuzzy_size_tolerance_pct = std::stod(next(arg.c_str()));
        else if (arg == "--polygon-block-time-sec") cfg->polygon_avg_block_time_sec = std::stod(next(arg.c_str()));
        else if (arg == "--output") cfg->output_path = next("--output");
        else if (arg == "--log-file") cfg->log_file = next("--log-file");
        else if (arg == "--log-interval-sec") cfg->log_interval_sec = std::stoi(next(arg.c_str()));
        else if (arg == "--worker-threads") cfg->worker_threads = std::stoi(next(arg.c_str()));
        else if (arg == "--queue-max-size") cfg->queue_max_size = std::stoi(next(arg.c_str()));
        else if (arg == "--force-unsynced-clock") cfg->force_unsynced_clock = true;
        else if (arg == "--dump-raw-messages") cfg->dump_raw_messages = std::stoi(next(arg.c_str()));
        else if (arg == "--dump-raw-path") cfg->dump_raw_path = next("--dump-raw-path");
        else if (arg == "--wallet-history-db") cfg->wallet_history_db_path = next(arg.c_str());
        else if (arg == "--anomaly-score-flag-threshold") cfg->anomaly_score_flag_threshold = std::stod(next(arg.c_str()));
        else if (arg == "--segment-low-max-trades") cfg->segment_low_max_trades = std::stoull(next(arg.c_str()));
        else if (arg == "--segment-low-max-trades-per-day") cfg->segment_low_max_trades_per_day = std::stod(next(arg.c_str()));
        else if (arg == "--segment-high-min-trades") cfg->segment_high_min_trades = std::stoull(next(arg.c_str()));
        else if (arg == "--segment-high-min-trades-per-day") cfg->segment_high_min_trades_per_day = std::stod(next(arg.c_str()));
        else if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return false; }
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); printUsage(argv[0]); return false; }
    }

    // --- validation ---
    if (cfg->auto_discover_assets && cfg->discover_by_category) {
        std::fprintf(stderr,
            "error: --auto-discover-assets and --discover-by-category are mutually exclusive "
            "(volume-based vs. category-based discovery are two different, incompatible ways to "
            "pick the subscription set) -- pass at most one\n");
        return false;
    }

    if ((cfg->auto_discover_assets || cfg->discover_by_category) && !cfg->asset_ids.empty()) {
        std::fprintf(stderr,
            "warning: --asset-ids set alongside a discovery mode; --asset-ids wins, "
            "discovery skipped\n");
        cfg->auto_discover_assets = false;
        cfg->discover_by_category = false;
    }

    if (cfg->discover_by_category && cfg->discover_tags.empty()) {
        std::fprintf(stderr, "error: --discover-by-category requires at least one tag in --discover-tags\n");
        return false;
    }

    // Category-resolved markets change composition far less often than the
    // 5-minute crypto markets volume-based discovery targets -- default to
    // a much longer refresh interval for this mode, but only if the user
    // hasn't already asked for a specific value (1800 is the compiled-in
    // volume-mode default; if it's still exactly that, nothing overrode it).
    if (cfg->discover_by_category && cfg->discover_refresh_interval_sec == 1800) {
        cfg->discover_refresh_interval_sec = 14400;
        std::fprintf(stderr,
            "info: --discover-by-category with no explicit --discover-refresh-interval-sec -- "
            "using 14400s (4h) instead of the volume-mode default of 1800s (30min), since "
            "category-resolved markets change far less often. Pass --discover-refresh-interval-sec "
            "explicitly to override.\n");
    }

    if (cfg->dump_raw_messages <= 0) {
        if (cfg->asset_ids.empty() && !cfg->auto_discover_assets && !cfg->discover_by_category) {
            std::fprintf(stderr,
                "error: --asset-ids, --auto-discover-assets, or --discover-by-category is required "
                "(comma-separated token ids to subscribe to, or pick them automatically)\n");
            return false;
        }
        if (cfg->match_mode != MatchMode::Off && cfg->rpc_url.empty()) {
            std::fprintf(stderr, "error: --rpc-url is required unless --match-mode off\n");
            return false;
        }
        if ((cfg->match_mode == MatchMode::Fuzzy || cfg->match_mode == MatchMode::Auto) &&
            (cfg->exchange_contract_address.empty() || cfg->order_filled_topic0.empty())) {
            if (cfg->match_mode == MatchMode::Fuzzy) {
                std::fprintf(stderr,
                    "error: --match-mode fuzzy requires --exchange-contract and --order-filled-topic0 "
                    "to be set explicitly -- this tool does not ship a hardcoded guess for either, "
                    "since a wrong value here would silently corrupt the matched lag numbers.\n");
                return false;
            }
            // Auto mode: fuzzy fallback simply stays disabled; tx-hash-only
            // matching still works. Logged clearly at startup in main.cpp.
        }
    } else {
        if (cfg->asset_ids.empty() && !cfg->auto_discover_assets && !cfg->discover_by_category) {
            std::fprintf(stderr,
                "error: --asset-ids, --auto-discover-assets, or --discover-by-category is required "
                "even in --dump-raw-messages mode\n");
            return false;
        }
    }

    return true;
}
