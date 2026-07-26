#include "resolve.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "logging.hpp"

using json = nlohmann::json;
using Clock = std::chrono::system_clock;

namespace {

bool stopped(const std::atomic<bool>* stopRequested) {
    return stopRequested != nullptr && stopRequested->load();
}

// Sleeps in short slices so a shutdown request interrupts the wait
// reasonably quickly instead of blocking for the full poll interval.
void interruptibleSleep(int totalMs, const std::atomic<bool>* stopRequested) {
    const int sliceMs = 200;
    int waited = 0;
    while (waited < totalMs) {
        if (stopped(stopRequested)) return;
        int thisSlice = std::min(sliceMs, totalMs - waited);
        std::this_thread::sleep_for(std::chrono::milliseconds(thisSlice));
        waited += thisSlice;
    }
}

double parseDoubleOr(const std::string& s, double fallback) {
    try {
        return std::stod(s);
    } catch (const std::exception&) {
        return fallback;
    }
}

// Decodes one eth_getLogs entry's price/size assuming Polymarket CTF
// Exchange's real OrderFilled layout, confirmed directly from
// Polymarket/ctf-exchange-v2's Events.sol/Trading.sol source (see
// wallet_resolve.cpp for the full reasoning): 3 indexed topics (orderHash,
// maker, taker) and data = side, tokenId, makerAmountFilled,
// takerAmountFilled, fee, builder, metadata (7 32-byte words; only the
// first four matter here). This does NOT verify tokenId/market identity,
// only price and size -- see the FuzzyLogScan doc comment above for why
// that's a known, deliberate limitation of this path specifically.
bool decodeOrderFilled(const json& log, double* outPrice, double* outSize) {
    if (!log.contains("data") || !log["data"].is_string()) return false;
    std::string data = log["data"].get<std::string>();
    if (data.size() > 2 && data[0] == '0' && (data[1] == 'x' || data[1] == 'X')) data = data.substr(2);
    if (data.size() < 4 * 64) return false;

    auto word = [&](int idx) { return data.substr(idx * 64, 64); };
    double makerAmount = hexWordToDouble(word(2));
    double takerAmount = hexWordToDouble(word(3));
    if (makerAmount <= 0 || takerAmount <= 0) return false;

    // USDC and Polymarket outcome tokens are both 6-decimal, so maker/taker
    // amounts scale the same way; nothing in the event says which side is
    // the collateral leg. Polymarket prices always fall in (0,1], and at
    // most one of the two orientations can (they're reciprocals of each
    // other), so picking whichever lands in that range is a deterministic,
    // not-guessed way to pick the right one.
    double priceA = makerAmount / takerAmount;
    if (priceA > 0 && priceA <= 1.0) {
        *outPrice = priceA;
        *outSize = takerAmount / 1e6;
    } else {
        *outPrice = takerAmount / makerAmount;
        *outSize = makerAmount / 1e6;
    }
    return true;
}

OnChainResolution resolveViaTxHash(PolygonRpcClient& client, const TradeEvent& trade,
                                    const AppConfig& cfg, const std::atomic<bool>* stopRequested) {
    OnChainResolution r;
    r.method = MatchMethod::TxHash;
    r.tx_hash = trade.tx_hash;

    for (int attempt = 0; attempt < cfg.rpc_poll_max_attempts; ++attempt) {
        if (stopped(stopRequested)) {
            r.note = "shutdown requested during tx-hash poll";
            r.wallet.note = "shutdown requested before a receipt was fetched";
            return r;
        }

        // eth_getTransactionReceipt instead of eth_getTransactionByHash --
        // same call slot (still gives blockNumber for the timestamp lookup
        // below), but its "logs" also let the wallet decode below happen
        // with no separate RPC call.
        auto receiptOpt = client.getTransactionReceipt(trade.tx_hash);
        if (receiptOpt && !receiptOpt->is_null() && receiptOpt->contains("blockNumber") &&
            !(*receiptOpt)["blockNumber"].is_null()) {
            std::string blockNumHex = (*receiptOpt)["blockNumber"].get<std::string>();

            r.wallet = resolveWalletFromLogs(receiptOpt->value("logs", json::array()), trade);

            auto blockOpt = client.getBlockByNumber(blockNumHex);
            if (blockOpt && blockOpt->contains("timestamp")) {
                r.resolved = true;
                r.block_number = hexToU64(blockNumHex);
                r.block_timestamp_unix = hexToU64((*blockOpt)["timestamp"].get<std::string>());
                return r;
            }
            r.note = "tx mined but block lookup failed";
            return r;
        }

        if (attempt + 1 < cfg.rpc_poll_max_attempts) {
            interruptibleSleep(cfg.rpc_poll_interval_ms, stopRequested);
        }
    }

    r.note = "tx not mined after " + std::to_string(cfg.rpc_poll_max_attempts) +
             " poll(s) (still pending, or hash not known to this node)";
    r.wallet.note = "no receipt available (tx never mined within the poll budget)";
    return r;
}

OnChainResolution resolveViaFuzzyLogScan(PolygonRpcClient& client, const TradeEvent& trade,
                                          const AppConfig& cfg, const std::atomic<bool>* stopRequested) {
    OnChainResolution r;
    r.method = MatchMethod::FuzzyLogScan;

    if (cfg.exchange_contract_address.empty() || cfg.order_filled_topic0.empty()) {
        r.note = "fuzzy match not configured (exchange_contract_address / order_filled_topic0 unset)";
        return r;
    }

    double wsPrice = parseDoubleOr(trade.price, -1.0);
    double wsSize = parseDoubleOr(trade.size, -1.0);
    if (wsPrice <= 0 || wsSize <= 0) {
        r.note = "could not parse price/size from WS payload for fuzzy matching";
        return r;
    }

    auto latestBlockOpt = client.getLatestBlockNumber();
    auto latestBlockJsonOpt = latestBlockOpt ? client.getBlockByNumber("latest") : std::nullopt;
    if (!latestBlockOpt || !latestBlockJsonOpt || !latestBlockJsonOpt->contains("timestamp")) {
        r.note = "could not fetch latest block to estimate a block range";
        return r;
    }
    uint64_t latestBlock = *latestBlockOpt;
    uint64_t latestTs = hexToU64((*latestBlockJsonOpt)["timestamp"].get<std::string>());

    int64_t tradeUnix = std::chrono::duration_cast<std::chrono::seconds>(
                            trade.recv_wall.time_since_epoch()).count();
    int64_t deltaSec = static_cast<int64_t>(latestTs) - tradeUnix;
    int64_t estBlocksAgo = deltaSec > 0
        ? static_cast<int64_t>(static_cast<double>(deltaSec) / cfg.polygon_avg_block_time_sec)
        : 0;
    uint64_t centerBlock = (estBlocksAgo < static_cast<int64_t>(latestBlock))
        ? latestBlock - static_cast<uint64_t>(estBlocksAgo)
        : 0;
    uint64_t fromBlock = (centerBlock > cfg.fuzzy_block_window) ? centerBlock - cfg.fuzzy_block_window : 0;
    uint64_t toBlock = std::min(latestBlock, centerBlock + cfg.fuzzy_block_window);

    if (stopped(stopRequested)) {
        r.note = "shutdown requested before fuzzy log scan";
        return r;
    }

    auto logsOpt = client.getLogs(cfg.exchange_contract_address, cfg.order_filled_topic0, fromBlock, toBlock);
    if (!logsOpt || !logsOpt->is_array()) {
        r.note = "eth_getLogs failed or returned non-array result";
        return r;
    }

    double bestScore = -1.0;
    std::string bestBlockHex, bestTxHash;
    for (const auto& log : *logsOpt) {
        double chainPrice = 0, chainSize = 0;
        if (!decodeOrderFilled(log, &chainPrice, &chainSize)) continue;

        double priceDiffPct = std::fabs(chainPrice - wsPrice) / wsPrice * 100.0;
        double sizeDiffPct = std::fabs(chainSize - wsSize) / wsSize * 100.0;
        if (priceDiffPct > cfg.fuzzy_price_tolerance_pct || sizeDiffPct > cfg.fuzzy_size_tolerance_pct) continue;

        double score = priceDiffPct + sizeDiffPct;
        if (bestScore < 0 || score < bestScore) {
            bestScore = score;
            bestBlockHex = log.value("blockNumber", "");
            bestTxHash = log.value("transactionHash", "");
        }
    }

    if (bestBlockHex.empty()) {
        r.note = "no fuzzy match within tolerance in block range [" +
                  std::to_string(fromBlock) + "," + std::to_string(toBlock) + "]";
        return r;
    }

    // Wallet decode reuses the same getLogs response already fetched above
    // -- no separate RPC call -- filtered down to just the winning tx's own
    // logs (a block-range scan spans many unrelated transactions).
    json matchedTxLogs = json::array();
    for (const auto& log : *logsOpt) {
        if (log.value("transactionHash", "") == bestTxHash) matchedTxLogs.push_back(log);
    }
    r.wallet = resolveWalletFromLogs(matchedTxLogs, trade);

    auto blockOpt = client.getBlockByNumber(bestBlockHex);
    if (!blockOpt || !blockOpt->contains("timestamp")) {
        r.note = "fuzzy match found but block timestamp lookup failed";
        return r;
    }

    r.resolved = true;
    r.tx_hash = bestTxHash;
    r.block_number = hexToU64(bestBlockHex);
    r.block_timestamp_unix = hexToU64((*blockOpt)["timestamp"].get<std::string>());
    r.note = "fuzzy match: price/size within tolerance, market identity NOT verified";
    return r;
}

} // namespace

OnChainResolution resolveTrade(PolygonRpcClient& client, const TradeEvent& trade,
                                const AppConfig& cfg, const std::atomic<bool>* stopRequested) {
    if (cfg.match_mode == MatchMode::Off) {
        OnChainResolution r;
        r.note = "match_mode=off";
        r.wallet.note = "match_mode=off, no receipt fetched to decode wallet from";
        return r;
    }

    bool haveTxHash = !trade.tx_hash.empty();
    bool tryTxHash = haveTxHash && (cfg.match_mode == MatchMode::Auto || cfg.match_mode == MatchMode::TxHash);
    bool fuzzyConfigured = !cfg.exchange_contract_address.empty() && !cfg.order_filled_topic0.empty();
    bool tryFuzzy = fuzzyConfigured && (cfg.match_mode == MatchMode::Fuzzy ||
                                         (cfg.match_mode == MatchMode::Auto && !haveTxHash));

    if (tryTxHash) {
        OnChainResolution r = resolveViaTxHash(client, trade, cfg, stopRequested);
        if (r.resolved) return r;
        // Auto mode: if tx-hash polling failed and fuzzy is configured, fall
        // back to it rather than giving up.
        if (cfg.match_mode == MatchMode::Auto && fuzzyConfigured && !stopped(stopRequested)) {
            OnChainResolution fallback = resolveViaFuzzyLogScan(client, trade, cfg, stopRequested);
            if (fallback.resolved) return fallback;
        }
        return r;
    }

    if (tryFuzzy) {
        return resolveViaFuzzyLogScan(client, trade, cfg, stopRequested);
    }

    OnChainResolution r;
    r.note = haveTxHash ? "match_mode excludes tx-hash path" : "no tx hash in payload and fuzzy match not configured/allowed";
    r.wallet.note = "no receipt/logs fetched to decode wallet from (" + r.note + ")";
    return r;
}
