#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// A trade-shaped message received over the WS, timestamped the instant it
// arrives (before any parsing/JSON work), plus whatever identifying fields
// we could pull out of the payload.
//
// Confirmed against a live capture (see README "Confirmed live payload
// schema"): Polymarket's CLOB market channel emits real trade fills as a
// top-level {"event_type":"last_trade_price", ...} object carrying a
// genuine "transaction_hash" field directly -- so tx-hash matching (the
// default match_mode) works as intended. "book" and "price_change" events
// also flow over the same channel and carry an unrelated, shorter internal
// "hash" field (order/book-state hash, ~20 bytes) that must NOT be
// confused with a transaction hash; parseTradeEvent() deliberately does not
// alias "hash" to tx_hash for this reason.
struct TradeEvent {
    std::chrono::system_clock::time_point recv_wall;
    std::chrono::steady_clock::time_point recv_mono;

    std::string raw_json;          // verbatim payload, kept for CSV + reprocessing
    std::string event_type;        // e.g. "last_trade_price" -- whatever "event_type"/"type" says
    std::string market;            // condition/market id, if present
    std::string asset_id;          // token id
    std::string side;
    std::string price;             // kept as the original string to avoid float round-tripping
    std::string size;
    std::string tx_hash;           // empty if the payload doesn't carry one
    std::string payload_timestamp; // Polymarket's own timestamp field, raw string, empty if absent
};

// Splits one raw WS text frame into its top-level JSON objects. Polymarket
// sometimes batches several objects into a single frame as a JSON array
// (observed on initial subscribe, when the server pushes one "book"
// snapshot per subscribed asset in one frame); a frame holding a single
// object is returned as a one-element vector. Returns an empty vector if
// the frame doesn't parse as JSON at all.
std::vector<nlohmann::json> splitTopLevelMessage(const std::string& rawJson);

// Returns true and fills *out if j looks enough like a trade fill (has both
// a price and a size field) to be worth resolving against on-chain data.
// recv_wall/recv_mono must already be set by the caller (captured at
// message-receipt time, not parse time); out->raw_json is set here from j.
bool parseTradeEvent(const nlohmann::json& j, TradeEvent* out);
