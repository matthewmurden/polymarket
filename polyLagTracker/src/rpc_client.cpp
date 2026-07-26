#include "rpc_client.hpp"

#include <curl/curl.h>

#include <cctype>
#include <cmath>
#include <cstdio>

#include "logging.hpp"

using json = nlohmann::json;

namespace {

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

PolygonRpcClient::PolygonRpcClient(std::string rpcUrl, long timeoutMs)
    : rpcUrl_(std::move(rpcUrl)), timeoutMs_(timeoutMs) {}

bool PolygonRpcClient::call(const std::string& method, const json& params, json* result) {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", method},
        {"params", params},
    };
    std::string body = req.dump();

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string respBody;
    curl_easy_setopt(curl, CURLOPT_URL, rpcUrl_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs_);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs_);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "poly-lag-tracker/1.0");

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        logging::error("rpc " + method + " transport error: " + curl_easy_strerror(res));
        return false;
    }
    if (status < 200 || status >= 300) {
        logging::error("rpc " + method + " http status " + std::to_string(status) + ": " + respBody);
        return false;
    }

    json resp;
    try {
        resp = json::parse(respBody);
    } catch (const json::parse_error& e) {
        logging::error("rpc " + method + " returned non-JSON body: " + e.what());
        return false;
    }

    if (resp.contains("error") && !resp["error"].is_null()) {
        logging::error("rpc " + method + " error response: " + resp["error"].dump());
        return false;
    }
    if (!resp.contains("result")) {
        logging::error("rpc " + method + " response missing 'result' field");
        return false;
    }

    *result = resp["result"];
    return true;
}

std::optional<json> PolygonRpcClient::getTransactionByHash(const std::string& txHash) {
    json result;
    if (!call("eth_getTransactionByHash", json::array({txHash}), &result)) return std::nullopt;
    return result;
}

std::optional<json> PolygonRpcClient::getBlockByNumber(const std::string& hexBlockNumber, bool fullTx) {
    json result;
    if (!call("eth_getBlockByNumber", json::array({hexBlockNumber, fullTx}), &result)) return std::nullopt;
    return result;
}

std::optional<json> PolygonRpcClient::getTransactionReceipt(const std::string& txHash) {
    json result;
    if (!call("eth_getTransactionReceipt", json::array({txHash}), &result)) return std::nullopt;
    return result;
}

std::optional<uint64_t> PolygonRpcClient::getLatestBlockNumber() {
    json result;
    if (!call("eth_blockNumber", json::array(), &result)) return std::nullopt;
    if (!result.is_string()) return std::nullopt;
    return hexToU64(result.get<std::string>());
}

std::optional<json> PolygonRpcClient::getLogs(const std::string& address, const std::string& topic0,
                                               uint64_t fromBlock, uint64_t toBlock) {
    json filter = {
        {"address", address},
        {"topics", json::array({topic0})},
        {"fromBlock", u64ToHex(fromBlock)},
        {"toBlock", u64ToHex(toBlock)},
    };
    json result;
    if (!call("eth_getLogs", json::array({filter}), &result)) return std::nullopt;
    return result;
}

uint64_t hexToU64(const std::string& hex) {
    if (hex.empty()) return 0;
    std::string h = hex;
    if (h.size() > 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    if (h.empty()) return 0;
    try {
        return std::stoull(h, nullptr, 16);
    } catch (const std::exception&) {
        return 0;
    }
}

std::string u64ToHex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

double hexWordToDouble(const std::string& word64Hex) {
    std::string h = word64Hex;
    if (h.size() > 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    // Strip leading zero bytes so the remaining hex fits in a double's
    // 53-bit exact-integer range for realistic on-chain amounts.
    size_t firstNonZero = h.find_first_not_of('0');
    if (firstNonZero == std::string::npos) return 0.0;
    std::string trimmed = h.substr(firstNonZero);
    if (trimmed.size() > 13) {
        // Too large to represent exactly as a double; approximate via
        // scientific accumulation rather than truncating silently wrong.
        double v = 0.0;
        for (char c : trimmed) {
            int digit = (c >= '0' && c <= '9') ? c - '0' : (std::tolower(c) - 'a' + 10);
            v = v * 16.0 + static_cast<double>(digit);
        }
        return v;
    }
    return static_cast<double>(std::stoull(trimmed, nullptr, 16));
}
