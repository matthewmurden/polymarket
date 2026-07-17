#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "config.hpp"
#include "rpc_client.hpp"
#include "trade_event.hpp"

enum class MatchMethod {
    TxHash,
    FuzzyLogScan,
    Unmatched,
};

struct OnChainResolution {
    MatchMethod method = MatchMethod::Unmatched;
    bool resolved = false;
    std::string tx_hash;              // filled in if resolved via either path
    uint64_t block_number = 0;
    uint64_t block_timestamp_unix = 0; // seconds
    std::string note;                  // human-readable reason when unresolved
};

// Attempts to resolve `trade`'s true on-chain execution time.
//
// - If trade.tx_hash is present and mode allows it (Auto/TxHash): polls
//   eth_getTransactionByHash up to cfg.rpc_poll_max_attempts times (a tx can
//   be seen by the WS before it's mined), then eth_getBlockByNumber for the
//   block timestamp.
// - Otherwise, if mode allows it (Auto/Fuzzy) and cfg.exchange_contract_address
//   / cfg.order_filled_topic0 are configured: scans OrderFilled-shaped logs
//   in a block range estimated from trade.recv_wall, and picks the closest
//   price/size match within tolerance. This path does NOT verify market/
//   asset identity (see README) -- treat FuzzyLogScan matches as lower
//   confidence than TxHash matches.
//
// `stopRequested`, if non-null, is polled between retries so a shutdown
// doesn't block on a long confirmation-poll loop.
OnChainResolution resolveTrade(PolygonRpcClient& client, const TradeEvent& trade,
                                const AppConfig& cfg, const std::atomic<bool>* stopRequested);
