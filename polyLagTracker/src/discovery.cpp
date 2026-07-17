#include "discovery.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>

#include "logging.hpp"

using json = nlohmann::json;

namespace {

const char* kTradesUrl = "https://data-api.polymarket.com/trades";
const int kRetryDelayMs = 1500;

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

// One GET attempt. Returns false on any transport/parse/content failure;
// *out is only meaningful when this returns true.
bool tryFetchOnce(int sampleSize, int topN, long timeoutMs, std::vector<DiscoveredAsset>* out) {
    std::string url = std::string(kTradesUrl) + "?limit=" + std::to_string(sampleSize);

    CURL* curl = curl_easy_init();
    if (!curl) {
        logging::error("discovery: curl_easy_init failed");
        return false;
    }

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

    if (res != CURLE_OK) {
        logging::error(std::string("discovery: GET ") + url + " transport error: " + curl_easy_strerror(res));
        return false;
    }
    if (status < 200 || status >= 300) {
        logging::error("discovery: GET " + url + " http status " + std::to_string(status));
        return false;
    }

    json trades;
    try {
        trades = json::parse(body);
    } catch (const json::parse_error& e) {
        logging::error(std::string("discovery: response was not valid JSON: ") + e.what());
        return false;
    }
    if (!trades.is_array()) {
        logging::error("discovery: expected a JSON array of trades, got something else");
        return false;
    }

    // Data API's field is "asset" (confirmed against a live response --
    // distinct from the WS payload's "asset_id" field name).
    std::unordered_map<std::string, int> counts;
    for (const auto& t : trades) {
        if (!t.is_object() || !t.contains("asset") || !t["asset"].is_string()) continue;
        ++counts[t["asset"].get<std::string>()];
    }
    if (counts.empty()) {
        logging::error("discovery: trade sample contained no usable 'asset' fields");
        return false;
    }

    std::vector<DiscoveredAsset> ranked;
    ranked.reserve(counts.size());
    for (auto& kv : counts) ranked.push_back({kv.first, kv.second});
    std::stable_sort(ranked.begin(), ranked.end(),
                      [](const DiscoveredAsset& a, const DiscoveredAsset& b) { return a.trade_count > b.trade_count; });
    if (static_cast<int>(ranked.size()) > topN) ranked.resize(topN);

    *out = std::move(ranked);
    return true;
}

} // namespace

std::optional<std::vector<DiscoveredAsset>> discoverActiveAssets(int sampleSize, int topN, long timeoutMs) {
    std::vector<DiscoveredAsset> result;
    if (tryFetchOnce(sampleSize, topN, timeoutMs, &result)) return result;

    logging::warn("discovery: first attempt failed, retrying once after " +
                  std::to_string(kRetryDelayMs) + "ms");
    std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));

    if (tryFetchOnce(sampleSize, topN, timeoutMs, &result)) return result;

    logging::error("discovery: second attempt also failed, giving up");
    return std::nullopt;
}
