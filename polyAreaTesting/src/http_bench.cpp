#include "http_bench.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <thread>
#include <chrono>

// Anonymous namespace: keeps writeCallback private to this file only,
// so it can't clash with same-named functions elsewhere in the project.
namespace {

// Called by curl automatically whenever a chunk of response data arrives.
// userdata is actually our std::string* (passed in via CURLOPT_WRITEDATA),
// but curl only knows it as a generic void* - we cast it back here.
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);

    // Append this chunk onto our string buffer.
    body->append(ptr, size * nmemb);

    // Must return the number of bytes handled - returning less than
    // size*nmemb signals a failure and makes curl abort the request.
    return size * nmemb;
}

}

bool performHttpGet(const std::string& url, long timeout_ms, std::string* body_out, HttpPhaseTimings* timing_out) {
    // Create a fresh curl "request object" to configure and send.
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());              // target address
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);         // give up after this long overall
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);  // give up if just connecting takes this long
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);                   // required for thread safety
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);             // follow redirects if the server sends one
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // negotiate compression, like a real client
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "poly-latency-bench/1.0"); // identify our traffic honestly

    // Force a fresh connection every time: no keepalive reuse across samples,
    // so DNS/connect/TLS timings are real on every single sample.
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

    // Route incoming response data to our writeCallback, which appends it to body_out.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body_out);

    // Actually send the request and wait for the full reply.
    CURLcode res = curl_easy_perform(curl);

    bool ok = false;
    if (res == CURLE_OK) { // true if no low-level error (timeout, DNS failure, connection refused, etc.)
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status); // the actual HTTP status code, e.g. 200 or 404

        // Five cumulative timestamps, all measured from the start of the request.
        double namelookup = 0, connect = 0, appconnect = 0, starttransfer = 0, total = 0;
        curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &namelookup);   // when DNS lookup finished
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect);         // when TCP connection was made
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &appconnect);   // when TLS handshake finished (0 if no TLS)
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &starttransfer); // when first reply byte arrived
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);             // when the whole request finished

        timing_out->http_status = status;

        // Convert cumulative timestamps into individual phase durations (seconds -> ms).
        timing_out->dns_ms = namelookup * 1000.0;
        timing_out->connect_ms = (connect - namelookup) * 1000.0;

        // If there was no TLS handshake (plain http), treat connect as the "TLS end" point too.
        double tlsEnd = appconnect > 0 ? appconnect : connect;
        timing_out->tls_ms = (tlsEnd - connect) * 1000.0;
        timing_out->ttfb_ms = (starttransfer - tlsEnd) * 1000.0;
        timing_out->total_ms = total * 1000.0;

        // Only a 2xx status counts as a genuinely successful sample.
        ok = (status >= 200 && status < 300);
    }

    curl_easy_cleanup(curl); // free the request object - every init() needs a matching cleanup()
    return ok;
}

HttpEndpointResult benchHttpEndpoint(const std::string& name, const std::string& url,
                                      int samples, int warmup, int delay_ms, long timeout_ms) {
    HttpEndpointResult result;
    result.name = name;
    result.url = url;

    std::vector<double> totals;                 // total_ms from every successful sample (needed for percentiles)
    double dnsSum = 0, connectSum = 0, tlsSum = 0, ttfbSum = 0; // running sums, for simple phase averages
    size_t okCount = 0;   // successful, non-warmup requests
    size_t failures = 0;  // failed, non-warmup requests
    size_t attempts = 0;  // total non-warmup requests made

    int totalRounds = warmup + samples;
    for (int i = 0; i < totalRounds; ++i) {
        std::string body;          // fresh empty container each iteration
        HttpPhaseTimings timing;   // fresh empty container each iteration
        bool ok = performHttpGet(url, timeout_ms, &body, &timing);

        bool isWarmup = i < warmup; // first `warmup` rounds are thrown away (cold DNS/TLS cache)

        if (!isWarmup) {
            ++attempts;
            if (ok) {
                // Record this sample's data for later stats.
                totals.push_back(timing.total_ms);
                dnsSum += timing.dns_ms;
                connectSum += timing.connect_ms;
                tlsSum += timing.tls_ms;
                ttfbSum += timing.ttfb_ms;
                ++okCount;
                result.last_body = body; // keep the most recent successful response body, for debugging
            } else {
                // Log the failure but keep going - one bad sample shouldn't kill the whole run.
                ++failures;
                std::fprintf(stderr, "[%s] request failed (attempt %d/%d), status=%ld\n",
                             name.c_str(), i - warmup + 1, samples, timing.http_status);
            }
        }

        // Pause between requests (except after the very last one) to be polite to the server.
        if (i != totalRounds - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }

    // Full percentile breakdown (min/mean/median/p95/p99/max) computed from the raw totals list.
    result.total_stats = computeStats(totals, attempts, failures);

    // Simple average per phase - guarded against divide-by-zero if every sample failed.
    if (okCount > 0) {
        result.mean_phases.dns_ms = dnsSum / static_cast<double>(okCount);
        result.mean_phases.connect_ms = connectSum / static_cast<double>(okCount);
        result.mean_phases.tls_ms = tlsSum / static_cast<double>(okCount);
        result.mean_phases.ttfb_ms = ttfbSum / static_cast<double>(okCount);
    }

    return result;
}