#include "wallet_history_fetch.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>

#include "logging.hpp"
#include "market_category.hpp"

using json = nlohmann::json;

namespace {

const char* kTradesUrl = "https://data-api.polymarket.com/trades";

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

std::pair<long, std::string> httpGet(const std::string& url, long timeoutMs) {
    CURL* curl = curl_easy_init();
    if (!curl) return {0, ""};

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "poly-lag-tracker/1.0");

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return {0, ""};
    return {status, body};
}

// Exactly "0x" + 40 hex chars -- confirmed live that anything else causes
// the Data API to silently fall back to unfiltered results instead of
// erroring, so this tool must not send it a malformed value in the first
// place.
bool isWellFormedAddress(const std::string& addr) {
    if (addr.size() != 42) return false;
    if (addr[0] != '0' || (addr[1] != 'x' && addr[1] != 'X')) return false;
    return std::all_of(addr.begin() + 2, addr.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

uint64_t nowUnix() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool stopped(const std::atomic<bool>* stopRequested) {
    return stopRequested != nullptr && stopRequested->load();
}

} // namespace

WalletHistoryRecord fetchWalletHistoryBackfill(const std::string& walletAddress, WalletHistoryStore* store,
                                                const WalletHistoryFetchConfig& cfg,
                                                const std::atomic<bool>* stopRequested) {
    WalletHistoryRecord rec;
    rec.wallet_address = walletAddress;

    if (!isWellFormedAddress(walletAddress)) {
        logging::error("wallet_history_fetch: refusing to query malformed address '" + walletAddress +
                       "' (must be 0x + 40 hex chars) -- the Data API silently returns unrelated "
                       "unfiltered trades for a bad `user` value rather than erroring, so this is never sent");
        return rec;  // known == false: caller should not cache this
    }

    std::string wanted = toLower(walletAddress);
    uint64_t totalFetched = 0;
    // Only true once we've actually determined the wallet's full history
    // (or hit the deliberate maxPages bound) -- NOT true if we bailed out
    // early due to shutdown or a transport/parse error. Distinguishing
    // these matters: a wallet that failed to look up must not be cached as
    // though it were confirmed to have zero trades.
    bool completedCleanly = false;

    for (int page = 0; page < cfg.maxPages; ++page) {
        if (stopped(stopRequested)) {
            logging::warn("wallet_history_fetch: shutdown requested mid-backfill for " + walletAddress +
                          " after " + std::to_string(totalFetched) + " trade(s); not caching this lookup");
            break;
        }

        std::string url = std::string(kTradesUrl) + "?user=" + walletAddress +
                           "&limit=" + std::to_string(cfg.pageSize) + "&offset=" + std::to_string(page * cfg.pageSize);
        auto [status, body] = httpGet(url, cfg.timeoutMs);
        if (status < 200 || status >= 300) {
            logging::error("wallet_history_fetch: GET " + url + " -> http " + std::to_string(status));
            break;
        }

        json trades;
        try {
            trades = json::parse(body);
        } catch (const json::parse_error& e) {
            logging::error(std::string("wallet_history_fetch: non-JSON response: ") + e.what());
            break;
        }
        if (!trades.is_array()) {
            logging::error("wallet_history_fetch: expected a JSON array of trades, got something else");
            break;
        }

        if (trades.empty()) {
            completedCleanly = true;
            break;  // clean end of this wallet's history
        }

        int mismatchCount = 0;
        for (const auto& t : trades) {
            if (!t.is_object()) continue;

            std::string proxyWallet = toLower(t.value("proxyWallet", std::string()));
            if (proxyWallet != wanted) {
                // The exact fallback-to-unfiltered-data failure mode
                // confirmed live in Part 1 -- discard this row rather than
                // attributing someone else's trade to this wallet.
                ++mismatchCount;
                continue;
            }

            double size = 0.0;
            if (t.contains("size")) {
                size = t["size"].is_string() ? std::stod(t["size"].get<std::string>()) : t["size"].get<double>();
            }
            uint64_t tradeUnix = 0;
            if (t.contains("timestamp")) {
                tradeUnix = t["timestamp"].is_string() ? std::stoull(t["timestamp"].get<std::string>())
                                                        : t["timestamp"].get<uint64_t>();
            }
            std::string conditionId = t.value("conditionId", std::string());

            // Cache-only, no network call: confirmed live that a wallet's
            // historical trades are mostly for already-expired markets, so
            // a network lookup per historical trade mostly just fails
            // while still costing a full round trip -- see
            // market_category.hpp for the load-test finding.
            std::string category = conditionId.empty() ? "unknown" : getCachedCategoryOnly(conditionId, store);
            foldTradeIntoHistory(&rec, size, category, tradeUnix);
            ++totalFetched;
        }

        if (mismatchCount > 0) {
            logging::error("wallet_history_fetch: " + std::to_string(mismatchCount) +
                           " row(s) in the response for " + walletAddress +
                           " had a different proxyWallet than queried -- discarded, not cached as this "
                           "wallet's history (endpoint likely fell back to unfiltered data)");
        }

        if (static_cast<int>(trades.size()) < cfg.pageSize) {
            completedCleanly = true;
            break;  // short page: no more pages
        }
        if (page + 1 >= cfg.maxPages) {
            logging::warn("wallet_history_fetch: hit maxPages=" + std::to_string(cfg.maxPages) + " for " +
                          walletAddress + " (very high trade count) -- history beyond this cap is not counted");
            completedCleanly = true;  // a deliberate bound, not a failure
        }
    }

    if (!completedCleanly) return rec;  // known == false: caller should not cache this

    rec.known = true;
    rec.had_zero_history_on_first_lookup = (totalFetched == 0);
    rec.last_updated_unix = nowUnix();
    return rec;
}
