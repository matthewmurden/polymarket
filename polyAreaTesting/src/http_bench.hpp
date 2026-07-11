#pragma once

#include <string>
#include <vector>

#include "stats.hpp"

// One completed (successful) GET, broken into the phases curl reports.
// Each phase is the time spent in that phase alone, not cumulative:
//   dns_ms      DNS resolution
//   connect_ms  TCP handshake, after DNS
//   tls_ms      TLS handshake, after TCP connect (0 on non-TLS)
//   ttfb_ms     server "think time" from TLS complete to first response byte
//   total_ms    full request, matches curl's TOTAL_TIME
struct HttpPhaseTimings {
    double dns_ms = 0;
    double connect_ms = 0;
    double tls_ms = 0;
    double ttfb_ms = 0;
    double total_ms = 0;
    long http_status = 0;
};

// Every sample opens a brand-new connection (CURLOPT_FRESH_CONNECT +
// CURLOPT_FORBID_REUSE) so DNS/connect/TLS are never zeroed out by
// keep-alive reuse. That's the number that matters for picking a region:
// how long a cold connection to Polymarket takes from that host.
bool performHttpGet(const std::string& url, long timeout_ms, std::string* body_out, HttpPhaseTimings* timing_out);

struct HttpEndpointResult {
    std::string name;
    std::string url;
    LatencyStats total_stats;       // stats over total_ms across samples
    HttpPhaseTimings mean_phases;   // mean of each phase across successful samples
    std::string last_body;          // body of the last successful response
};

HttpEndpointResult benchHttpEndpoint(const std::string& name, const std::string& url,
                                      int samples, int warmup, int delay_ms, long timeout_ms);
