// Standalone latency benchmark against Polymarket's public API surface.
// Not the bot itself -- this exists purely to compare cloud regions before
// picking where to deploy the real trade-monitoring service.
//
// Measures:
//   - CLOB REST   GET https://clob.polymarket.com/time
//   - Data API    GET https://data-api.polymarket.com/trades?limit=1
//   - Gamma API   GET https://gamma-api.polymarket.com/events?limit=1
//   - CLOB WS     wss://ws-subscriptions-clob.polymarket.com/ws/market
//                 (handshake time, and time-to-first-message after subscribe)
//
// All four are public, unauthenticated, read-only endpoints.

#include <curl/curl.h>
#include <ixwebsocket/IXNetSystem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <unistd.h>

#include "http_bench.hpp"
#include "ws_bench.hpp"

namespace {

struct Config {
    int samples = 100;
    int warmup = 5;
    int delayMs = 200;
    long httpTimeoutMs = 5000;
    int wsConnectTimeoutMs = 5000;
    int wsMessageTimeoutMs = 10000;
    std::string tokenIdOverride;
    std::string outputPath;
};

void printUsage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "  --samples N              samples per endpoint after warmup (default 100)\n"
        "  --warmup N                warmup samples discarded before recording (default 5)\n"
        "  --delay-ms N               delay between samples, ms (default 200)\n"
        "  --http-timeout-ms N        per-request timeout for REST calls (default 5000)\n"
        "  --ws-connect-timeout-ms N  handshake timeout for WS connect (default 5000)\n"
        "  --ws-message-timeout-ms N  timeout waiting for first WS message (default 10000)\n"
        "  --token-id ID              force the asset/token id used for WS subscribe\n"
        "                             (default: auto-fetched from a live trade)\n"
        "  --output PATH              CSV output path (default ./results_<region>_<ts>.csv)\n"
        "  --help                     show this message\n"
        "\n"
        "Region label for CSV output comes from the BENCH_REGION_LABEL env var,\n"
        "falling back to the machine hostname.\n",
        prog);
}

bool parseArgs(int argc, char** argv, Config* cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--samples") cfg->samples = std::stoi(next("--samples"));
        else if (arg == "--warmup") cfg->warmup = std::stoi(next("--warmup"));
        else if (arg == "--delay-ms") cfg->delayMs = std::stoi(next("--delay-ms"));
        else if (arg == "--http-timeout-ms") cfg->httpTimeoutMs = std::stol(next("--http-timeout-ms"));
        else if (arg == "--ws-connect-timeout-ms") cfg->wsConnectTimeoutMs = std::stoi(next("--ws-connect-timeout-ms"));
        else if (arg == "--ws-message-timeout-ms") cfg->wsMessageTimeoutMs = std::stoi(next("--ws-message-timeout-ms"));
        else if (arg == "--token-id") cfg->tokenIdOverride = next("--token-id");
        else if (arg == "--output") cfg->outputPath = next("--output");
        else if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return false; }
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); printUsage(argv[0]); return false; }
    }
    return true;
}

std::string regionLabel() {
    if (const char* env = std::getenv("BENCH_REGION_LABEL")) {
        if (std::strlen(env) > 0) return env;
    }
    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) == 0) return host;
    return "unknown-region";
}

std::string isoTimestampUtc() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

// Minimal, dependency-free extraction of the first `"asset":"..."` value
// from a Data API /trades response, used to find a live token id to
// subscribe to for the WS benchmark. Not a general JSON parser.
std::string extractFirstAssetId(const std::string& json) {
    const std::string key = "\"asset\":\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

void printHttpRow(const HttpEndpointResult& r) {
    const auto& s = r.total_stats;
    std::printf("%-14s %8zu %8zu %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f\n",
                r.name.c_str(), s.attempts, s.failures,
                s.min_ms, s.mean_ms, s.median_ms, s.p95_ms, s.p99_ms, s.max_ms);
}

void printWsRow(const std::string& label, const LatencyStats& s) {
    std::printf("%-14s %8zu %8zu %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f\n",
                label.c_str(), s.attempts, s.failures,
                s.min_ms, s.mean_ms, s.median_ms, s.p95_ms, s.p99_ms, s.max_ms);
}

void writeCsvHttpRow(std::ofstream& out, const std::string& ts, const std::string& region,
                      const HttpEndpointResult& r) {
    const auto& s = r.total_stats;
    out << ts << "," << region << "," << r.name << "," << r.url << ",total,"
        << s.attempts << "," << s.failures << ","
        << s.min_ms << "," << s.mean_ms << "," << s.median_ms << ","
        << s.p95_ms << "," << s.p99_ms << "," << s.max_ms << ","
        << r.mean_phases.dns_ms << "," << r.mean_phases.connect_ms << ","
        << r.mean_phases.tls_ms << "," << r.mean_phases.ttfb_ms << "\n";
}

void writeCsvWsRow(std::ofstream& out, const std::string& ts, const std::string& region,
                    const std::string& name, const std::string& url, const std::string& metric,
                    const LatencyStats& s) {
    out << ts << "," << region << "," << name << "," << url << "," << metric << ","
        << s.attempts << "," << s.failures << ","
        << s.min_ms << "," << s.mean_ms << "," << s.median_ms << ","
        << s.p95_ms << "," << s.p99_ms << "," << s.max_ms << ",,,,\n"; // no phase breakdown for WS
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parseArgs(argc, argv, &cfg)) return 1;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    ix::initNetSystem();

    const std::string region = regionLabel();
    const std::string ts = isoTimestampUtc();

    std::printf("Polymarket API latency benchmark\n");
    std::printf("region=%s  samples=%d  warmup=%d  delay=%dms  time=%s\n\n",
                region.c_str(), cfg.samples, cfg.warmup, cfg.delayMs, ts.c_str());

    // --- REST endpoints ---------------------------------------------------
    HttpEndpointResult clobTime = benchHttpEndpoint(
        "clob_time", "https://clob.polymarket.com/time",
        cfg.samples, cfg.warmup, cfg.delayMs, cfg.httpTimeoutMs);

    HttpEndpointResult dataTrades = benchHttpEndpoint(
        "data_trades", "https://data-api.polymarket.com/trades?limit=1",
        cfg.samples, cfg.warmup, cfg.delayMs, cfg.httpTimeoutMs);

    HttpEndpointResult gammaEvents = benchHttpEndpoint(
        "gamma_events", "https://gamma-api.polymarket.com/events?limit=1",
        cfg.samples, cfg.warmup, cfg.delayMs, cfg.httpTimeoutMs);

    // --- resolve a live token id for the WS subscribe test ----------------
    std::string tokenId = cfg.tokenIdOverride;
    if (tokenId.empty()) tokenId = extractFirstAssetId(dataTrades.last_body);
    if (tokenId.empty()) {
        std::fprintf(stderr,
            "warning: could not auto-resolve a live token id from the Data API; "
            "re-run with --token-id to set one explicitly. Skipping WS benchmark.\n");
    }

    WsEndpointResult wsResult;
    bool ranWs = false;
    if (!tokenId.empty()) {
        std::printf("WS subscribe test using token_id=%s\n\n", tokenId.c_str());
        wsResult = benchWsEndpoint(
            "clob_ws", "wss://ws-subscriptions-clob.polymarket.com/ws/market", tokenId,
            cfg.samples, cfg.warmup, cfg.delayMs, cfg.wsConnectTimeoutMs, cfg.wsMessageTimeoutMs);
        ranWs = true;
    }

    // --- human-readable summary --------------------------------------------
    std::printf("%-14s %8s %8s %9s %9s %9s %9s %9s %9s\n",
                "endpoint", "samples", "fail", "min_ms", "mean_ms", "median_ms", "p95_ms", "p99_ms", "max_ms");
    printHttpRow(clobTime);
    printHttpRow(dataTrades);
    printHttpRow(gammaEvents);
    if (ranWs) {
        printWsRow("ws_handshake", wsResult.handshake_stats);
        printWsRow("ws_first_msg", wsResult.first_message_stats);
    }

    std::printf("\nREST phase breakdown (mean, ms): dns / connect / tls / ttfb\n");
    for (const auto* r : {&clobTime, &dataTrades, &gammaEvents}) {
        std::printf("%-14s %8.1f %8.1f %8.1f %8.1f\n", r->name.c_str(),
                    r->mean_phases.dns_ms, r->mean_phases.connect_ms,
                    r->mean_phases.tls_ms, r->mean_phases.ttfb_ms);
    }

    // --- CSV -----------------------------------------------------------
    std::string outPath = cfg.outputPath;
    if (outPath.empty()) {
        outPath = "results_" + region + "_" + std::to_string(std::time(nullptr)) + ".csv";
    }
    std::ofstream csv(outPath);
    if (!csv) {
        std::fprintf(stderr, "failed to open CSV output path: %s\n", outPath.c_str());
    } else {
        csv << "timestamp,region,endpoint,url,metric,attempts,failures,"
               "min_ms,mean_ms,median_ms,p95_ms,p99_ms,max_ms,"
               "dns_mean_ms,connect_mean_ms,tls_mean_ms,ttfb_mean_ms\n";
        writeCsvHttpRow(csv, ts, region, clobTime);
        writeCsvHttpRow(csv, ts, region, dataTrades);
        writeCsvHttpRow(csv, ts, region, gammaEvents);
        if (ranWs) {
            writeCsvWsRow(csv, ts, region, "clob_ws",
                          "wss://ws-subscriptions-clob.polymarket.com/ws/market",
                          "handshake", wsResult.handshake_stats);
            writeCsvWsRow(csv, ts, region, "clob_ws",
                          "wss://ws-subscriptions-clob.polymarket.com/ws/market",
                          "first_message", wsResult.first_message_stats);
        }
        std::printf("\nwrote %s\n", outPath.c_str());
    }

    ix::uninitNetSystem();
    curl_global_cleanup();
    return 0;
}
