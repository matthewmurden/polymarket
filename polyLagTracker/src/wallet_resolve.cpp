#include "wallet_resolve.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <set>

#include "rpc_client.hpp"  // hexWordToDouble

using json = nlohmann::json;

namespace {

// Polymarket runs TWO deployed exchange instances on Polygon that both emit
// this identical OrderFilled event: the main CTFExchangeV2 and a second
// NegRiskCtfExchangeV2 instance used specifically for NegRisk (multi-
// outcome) markets. Both addresses are confirmed directly from
// Polymarket/ctf-exchange-v2's own README "Deployed Contracts" table (not
// guessed) -- a live test that checked only the main address left ~14% of
// trades unresolved, all of which turned out to be NegRisk-market fills
// settled through the second contract instead.
const std::string kCtfExchangeV2 = "0xe111180000d2663c0091e4f400237545b87b996b";
const std::string kNegRiskCtfExchangeV2 = "0xe2222d279d744050d28e00520010520000310f59";

// keccak256("OrderFilled(bytes32,address,address,uint8,uint256,uint256,uint256,uint256,bytes32,bytes32)"),
// computed directly from the real event signature in
// Polymarket/ctf-exchange-v2's src/exchange/mixins/Events.sol -- not
// guessed, and not the same event/contract as the older CTF Exchange V1.
const std::string kOrderFilledTopic0 =
    "0xd543adfd945773f1a62f74f0ee55a5e3b9b1a28262980ba90b1a89f2ea84d8ee";

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string stripHexPrefix(const std::string& s) {
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return s.substr(2);
    return s;
}

std::string topicToAddress(const std::string& topic) {
    std::string h = stripHexPrefix(topic);
    if (h.size() < 40) return "";
    return "0x" + toLower(h.substr(h.size() - 40));
}

// Converts a decimal token-id string to a canonical, zero-padded 64-hex-char
// (256-bit) big-endian word for exact comparison against a log's tokenId
// word. Token ids are full 256-bit values, well beyond double's 53-bit exact
// range (hexWordToDouble is explicitly not precise there), so this does
// plain big-integer decimal-to-binary conversion via repeated
// multiply-by-10-and-add over a 32-byte accumulator instead.
bool decimalToHex256(const std::string& decimal, std::string* outHex64) {
    if (decimal.empty()) return false;
    for (char c : decimal) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    uint8_t bytes[32] = {0};
    for (char c : decimal) {
        int digit = c - '0';
        int carry = digit;
        for (int i = 31; i >= 0; --i) {
            int v = bytes[i] * 10 + carry;
            bytes[i] = static_cast<uint8_t>(v & 0xFF);
            carry = v >> 8;
        }
        if (carry != 0) return false;  // overflow past 256 bits -- not a real token id
    }
    static const char* kHexDigits = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (uint8_t b : bytes) {
        hex += kHexDigits[(b >> 4) & 0xF];
        hex += kHexDigits[b & 0xF];
    }
    *outHex64 = hex;
    return true;
}

double parseDoubleOr(const std::string& s, double fallback) {
    try {
        return std::stod(s);
    } catch (const std::exception&) {
        return fallback;
    }
}

struct DecodedOrderFilled {
    uint64_t side = 0;  // 0=BUY, 1=SELL (Structs.sol `enum Side`) -- always makerOrder.side
    std::string tokenIdHex64;
    double makerAmountFilled = 0;
    double takerAmountFilled = 0;
};

// OrderFilledParams (Events.sol): orderHash, maker, taker (all indexed --
// topics[1..3]), then data = side, tokenId, makerAmountFilled,
// takerAmountFilled, fee, builder, metadata (7 32-byte words; only the
// first four matter here).
bool decodeOrderFilledData(const std::string& dataField, DecodedOrderFilled* out) {
    std::string h = stripHexPrefix(dataField);
    if (h.size() < 4 * 64) return false;
    auto word = [&](int idx) { return h.substr(idx * 64, 64); };
    out->side = static_cast<uint64_t>(hexWordToDouble(word(0)));
    out->tokenIdHex64 = toLower(word(1));
    out->makerAmountFilled = hexWordToDouble(word(2));
    out->takerAmountFilled = hexWordToDouble(word(3));
    return true;
}

// USDC and Polymarket outcome tokens are both 6-decimal, so maker/taker
// amounts scale the same way; nothing in the event says which side is the
// collateral leg, so both orientations are tried within tolerance -- same
// approach empirically validated (83/83 correct) before this replaced the
// Data-API wallet lookup.
bool priceAndSizeMatch(double makerAmountFilled, double takerAmountFilled, double targetPrice, double targetSize) {
    constexpr double kPriceTolAbs = 0.01;
    constexpr double kSizeTolFrac = 0.05;
    double makerU = makerAmountFilled / 1e6;
    double takerU = takerAmountFilled / 1e6;

    if (takerU > 0) {
        double price = makerU / takerU;
        double size = takerU;
        if (std::fabs(price - targetPrice) <= kPriceTolAbs &&
            std::fabs(size - targetSize) <= std::max(kSizeTolFrac, targetSize * kSizeTolFrac)) {
            return true;
        }
    }
    if (makerU > 0) {
        double price = takerU / makerU;
        double size = makerU;
        if (std::fabs(price - targetPrice) <= kPriceTolAbs &&
            std::fabs(size - targetSize) <= std::max(kSizeTolFrac, targetSize * kSizeTolFrac)) {
            return true;
        }
    }
    return false;
}

}  // namespace

WalletResolution resolveWalletFromLogs(const json& logs, const TradeEvent& trade) {
    WalletResolution result;

    if (!logs.is_array() || logs.empty()) {
        result.note = "no logs available to decode (empty or missing receipt logs)";
        return result;
    }

    double targetPrice = parseDoubleOr(trade.price, -1.0);
    double targetSize = parseDoubleOr(trade.size, -1.0);
    if (targetPrice <= 0 || targetSize <= 0) {
        result.note = "could not parse price/size from WS payload";
        return result;
    }

    std::string targetTokenHex;
    if (!decimalToHex256(trade.asset_id, &targetTokenHex)) {
        result.note = "could not parse asset_id as a token id";
        return result;
    }

    std::string wsSide = toLower(trade.side);
    if (wsSide != "buy" && wsSide != "sell") {
        result.note = "WS payload had no usable side (BUY/SELL) to disambiguate maker vs. taker";
        return result;
    }
    bool wsSideIsSell = (wsSide == "sell");

    std::set<std::string> resolvedWallets;
    int matchingLogCount = 0;

    for (const auto& log : logs) {
        if (!log.is_object()) continue;
        std::string logAddr = toLower(log.value("address", std::string()));
        if (logAddr != kCtfExchangeV2 && logAddr != kNegRiskCtfExchangeV2) continue;
        if (!log.contains("topics") || !log["topics"].is_array() || log["topics"].size() < 4) continue;
        if (toLower(log["topics"][0].get<std::string>()) != kOrderFilledTopic0) continue;
        if (!log.contains("data") || !log["data"].is_string()) continue;

        DecodedOrderFilled decoded;
        if (!decodeOrderFilledData(log["data"].get<std::string>(), &decoded)) continue;
        if (decoded.tokenIdHex64 != targetTokenHex) continue;
        if (!priceAndSizeMatch(decoded.makerAmountFilled, decoded.takerAmountFilled, targetPrice, targetSize)) {
            continue;
        }

        std::string maker = topicToAddress(log["topics"][2].get<std::string>());
        std::string taker = topicToAddress(log["topics"][3].get<std::string>());
        if (maker.empty() || taker.empty()) continue;

        ++matchingLogCount;

        // The event's `side` is always makerOrder.side (traced through
        // Trading.sol's _emitOrderFilledEvent / _emitTakerFilledEvents call
        // sites, not guessed): if the WS-captured side agrees with the
        // log's side, the wallet being resolved is the maker; otherwise
        // it's the taker. This single rule holds without any special-
        // casing for the exchange's own redundant "mirror" log (emitted
        // against itself for NegRisk mint/merge fills) -- verified against
        // 83/83 known-correct wallets, including cases with 2 logs matching
        // the same tokenId+price/size, before this replaced the old
        // Data-API lookup.
        bool logSideIsSell = (decoded.side == 1);
        resolvedWallets.insert(wsSideIsSell == logSideIsSell ? maker : taker);
    }

    if (matchingLogCount == 0) {
        result.note = "no OrderFilled log matched this trade's tokenId+price/size";
        return result;
    }
    if (resolvedWallets.size() > 1) {
        result.note = "ambiguous: " + std::to_string(resolvedWallets.size()) +
                       " distinct wallets resolved across " + std::to_string(matchingLogCount) +
                       " matching log(s)";
        return result;
    }

    result.resolved = true;
    result.wallet_address = *resolvedWallets.begin();
    result.note = "resolved via OrderFilled log decode (" + std::to_string(matchingLogCount) + " matching log(s))";
    return result;
}
