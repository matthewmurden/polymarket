#include "category_discovery.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <unordered_map>

#include "logging.hpp"

using json = nlohmann::json;

namespace {

const char* kEventsUrl = "https://gamma-api.polymarket.com/events";
const int kPageSize = 100;  // confirmed live: /events ignores limit values above 100
const int kRetryDelayMs = 1000;

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

// One GET /events?tag_slug=<tag>&closed=false&limit=100&offset=<offset>.
// Returns nullopt on any transport/HTTP/parse failure (caller retries
// once); an empty-but-successful page is a real, valid "no more events"
// signal, not a failure.
std::optional<json> fetchEventsPage(const std::string& tag, int offset, long timeoutMs) {
    std::string url = std::string(kEventsUrl) + "?tag_slug=" + tag + "&closed=false&limit=" +
                       std::to_string(kPageSize) + "&offset=" + std::to_string(offset);
    auto [status, body] = httpGet(url, timeoutMs);
    if (status < 200 || status >= 300) return std::nullopt;

    try {
        json page = json::parse(body);
        if (!page.is_array()) return std::nullopt;
        return page;
    } catch (const json::parse_error&) {
        return std::nullopt;
    }
}

// Pages through every open event for one tag, retrying a failed page once
// after a short delay before giving up on that tag (logged either way).
std::vector<json> fetchAllEventsForTag(const std::string& tag, long timeoutMs) {
    std::vector<json> events;
    int offset = 0;
    for (;;) {
        auto page = fetchEventsPage(tag, offset, timeoutMs);
        if (!page) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
            page = fetchEventsPage(tag, offset, timeoutMs);
        }
        if (!page) {
            logging::error("category_discovery: giving up on tag_slug=" + tag + " at offset=" +
                           std::to_string(offset) + " after a retry");
            break;
        }
        if (page->empty()) break;

        for (const auto& ev : *page) events.push_back(ev);
        if (static_cast<int>(page->size()) < kPageSize) break;
        offset += kPageSize;
    }
    return events;
}

} // namespace

std::optional<std::vector<CategoryDiscoveredAsset>> discoverAssetsByCategory(
    const std::vector<std::string>& tagSlugs, long timeoutMs) {
    if (tagSlugs.empty()) {
        logging::error("category_discovery: no tag slugs configured");
        return std::nullopt;
    }

    // Dedup events by id -- the same event can carry several queried tags.
    // Value keeps the FIRST tag it was found under, purely for logging/
    // debugging context on the resulting asset list.
    std::unordered_map<std::string, std::pair<json, std::string>> eventsById;

    for (const auto& tag : tagSlugs) {
        std::vector<json> events = fetchAllEventsForTag(tag, timeoutMs);
        int newForThisTag = 0;
        for (auto& ev : events) {
            if (!ev.contains("id")) continue;
            std::string id = ev["id"].is_string() ? ev["id"].get<std::string>() : ev["id"].dump();
            if (eventsById.find(id) == eventsById.end()) {
                ++newForThisTag;
                eventsById.emplace(id, std::make_pair(std::move(ev), tag));
            }
        }
        logging::info("category_discovery: tag_slug=" + tag + " -> " + std::to_string(events.size()) +
                      " open event(s), " + std::to_string(newForThisTag) + " new (not already seen "
                      "under a different tag)");
    }

    if (eventsById.empty()) {
        logging::error("category_discovery: zero open events found across all configured tags -- "
                       "check --discover-tags against the real /tags taxonomy (gamma-api.polymarket.com)");
        return std::nullopt;
    }

    std::vector<CategoryDiscoveredAsset> result;
    int marketsSeen = 0;
    int marketsMissingTokenIds = 0;

    for (const auto& [id, evAndTag] : eventsById) {
        const json& ev = evAndTag.first;
        const std::string& tag = evAndTag.second;
        std::string eventTitle = ev.value("title", std::string());
        if (!ev.contains("markets") || !ev["markets"].is_array()) continue;

        for (const auto& market : ev["markets"]) {
            // Market-level closed check -- see header doc comment for why
            // this can't be skipped in favor of trusting the event-level
            // closed=false query filter alone.
            if (market.value("closed", true)) continue;
            ++marketsSeen;

            if (!market.contains("clobTokenIds") || !market["clobTokenIds"].is_string()) {
                ++marketsMissingTokenIds;
                continue;
            }
            std::string conditionId = market.value("conditionId", std::string());
            try {
                json tokenIds = json::parse(market["clobTokenIds"].get<std::string>());
                if (!tokenIds.is_array()) {
                    ++marketsMissingTokenIds;
                    continue;
                }
                for (const auto& tokenId : tokenIds) {
                    if (!tokenId.is_string()) continue;
                    result.push_back(CategoryDiscoveredAsset{tokenId.get<std::string>(), conditionId,
                                                              eventTitle, tag});
                }
            } catch (const json::parse_error&) {
                ++marketsMissingTokenIds;
            }
        }
    }

    logging::info("category_discovery: " + std::to_string(eventsById.size()) + " unique open event(s) -> " +
                  std::to_string(marketsSeen) + " open market(s) -> " + std::to_string(result.size()) +
                  " outcome token(s) to subscribe" +
                  (marketsMissingTokenIds > 0
                       ? " (" + std::to_string(marketsMissingTokenIds) + " market(s) skipped: missing/"
                         "malformed clobTokenIds)"
                       : ""));

    if (result.empty()) {
        logging::error("category_discovery: found open events but zero usable outcome tokens");
        return std::nullopt;
    }

    return result;
}
