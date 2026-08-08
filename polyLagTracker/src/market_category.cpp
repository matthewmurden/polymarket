#include "market_category.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <limits>

#include "logging.hpp"

using json = nlohmann::json;

namespace {

const char* kMarketsUrl = "https://gamma-api.polymarket.com/markets";
const char* kEventsUrl = "https://gamma-api.polymarket.com/events";
const char* kUnknownCategory = "unknown";

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

// Returns {statusCode, body}; statusCode 0 means a transport-level failure.
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

// GET /markets?condition_ids=<id> -> the embedded events[0].id, or empty on
// any failure (network, non-200, non-JSON, no market/event found).
std::string fetchEventIdForCondition(const std::string& conditionId, long timeoutMs) {
    std::string url = std::string(kMarketsUrl) + "?condition_ids=" + conditionId;
    auto [status, body] = httpGet(url, timeoutMs);
    if (status < 200 || status >= 300) return "";

    try {
        json markets = json::parse(body);
        if (!markets.is_array() || markets.empty()) return "";
        const json& market = markets[0];
        if (!market.contains("events") || !market["events"].is_array() || market["events"].empty()) return "";
        const json& event = market["events"][0];
        if (!event.contains("id")) return "";
        return event["id"].is_string() ? event["id"].get<std::string>() : event["id"].dump();
    } catch (const json::exception&) {
        return "";
    }
}

// GET /events?id=<id> -> the label of the tag with the lowest numeric id
// among tags[], or "" on any failure/no tags.
std::string fetchLowestIdTagLabel(const std::string& eventId, long timeoutMs) {
    std::string url = std::string(kEventsUrl) + "?id=" + eventId;
    auto [status, body] = httpGet(url, timeoutMs);
    if (status < 200 || status >= 300) return "";

    try {
        json events = json::parse(body);
        if (!events.is_array() || events.empty()) return "";
        const json& event = events[0];
        if (!event.contains("tags") || !event["tags"].is_array() || event["tags"].empty()) return "";

        std::string bestLabel;
        long long bestId = std::numeric_limits<long long>::max();
        for (const auto& tag : event["tags"]) {
            if (!tag.contains("id") || !tag.contains("label")) continue;
            long long tagId;
            try {
                tagId = tag["id"].is_string() ? std::stoll(tag["id"].get<std::string>()) : tag["id"].get<long long>();
            } catch (const std::exception&) {
                continue;
            }
            if (tagId < bestId) {
                bestId = tagId;
                bestLabel = tag["label"].get<std::string>();
            }
        }
        return bestLabel;
    } catch (const json::exception&) {
        return "";
    }
}

} // namespace

std::string resolveMarketCategory(const std::string& conditionId, WalletHistoryStore* store, long timeoutMs) {
    if (auto cached = store->getCachedCategory(conditionId)) {
        return *cached;
    }

    std::string category = kUnknownCategory;
    std::string eventId = fetchEventIdForCondition(conditionId, timeoutMs);
    if (!eventId.empty()) {
        std::string label = fetchLowestIdTagLabel(eventId, timeoutMs);
        if (!label.empty()) category = label;
    }

    if (category == kUnknownCategory) {
        logging::warn("market_category: could not resolve a category for conditionId=" + conditionId +
                      " (market may have already expired/been pruned from Gamma's index) -- caching as 'unknown'");
    }

    store->cacheCategory(conditionId, category);
    return category;
}

std::string getCachedCategoryOnly(const std::string& conditionId, WalletHistoryStore* store) {
    if (auto cached = store->getCachedCategory(conditionId)) {
        return *cached;
    }
    return kUnknownCategory;
}
