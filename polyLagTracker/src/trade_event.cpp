#include "trade_event.hpp"

using json = nlohmann::json;

namespace {

// Polymarket's JSON fields are usually strings even for numeric values
// (e.g. "price": "0.53"), but this tolerates a raw number too.
std::string asString(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number_integer()) return std::to_string(j.get<long long>());
    if (j.is_number_unsigned()) return std::to_string(j.get<unsigned long long>());
    if (j.is_number_float()) return std::to_string(j.get<double>());
    if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
    return "";
}

std::string firstOf(const json& j, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (j.contains(k) && !j[k].is_null()) return asString(j[k]);
    }
    return "";
}

} // namespace

std::vector<json> splitTopLevelMessage(const std::string& rawJson) {
    json j;
    try {
        j = json::parse(rawJson);
    } catch (const json::parse_error&) {
        return {};
    }
    if (j.is_array()) {
        std::vector<json> out;
        out.reserve(j.size());
        for (auto& elem : j) {
            if (elem.is_object()) out.push_back(elem);
        }
        return out;
    }
    if (j.is_object()) return {j};
    return {};
}

bool parseTradeEvent(const json& j, TradeEvent* out) {
    out->raw_json = j.dump();

    out->event_type = firstOf(j, {"event_type", "type"});
    out->market = firstOf(j, {"market", "condition_id", "conditionId"});
    out->asset_id = firstOf(j, {"asset_id", "asset", "token_id", "tokenId"});
    out->side = firstOf(j, {"side"});
    out->price = firstOf(j, {"price"});
    out->size = firstOf(j, {"size", "amount"});
    // Deliberately does NOT alias the bare "hash" key here -- confirmed via
    // a live capture that "book"/"price_change" events carry an unrelated,
    // shorter internal hash under that same key. Real trade fills
    // (event_type == "last_trade_price") carry a genuine transaction hash
    // under "transaction_hash".
    out->tx_hash = firstOf(j, {"transaction_hash", "transactionHash", "tx_hash", "txHash"});
    out->payload_timestamp = firstOf(j, {"timestamp", "time", "ts"});

    // Only worth queueing for on-chain resolution if it actually looks like
    // a trade fill (has both a price and a size) rather than e.g. a book
    // snapshot or a price_change batch (whose per-entry data lives in a
    // nested array this function doesn't look inside).
    return !out->price.empty() && !out->size.empty();
}
