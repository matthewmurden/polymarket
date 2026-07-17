// poly_lag_tracker: measures the real, steady-state lag between a trade's
// true on-chain execution time and the moment it's delivered over
// Polymarket's CLOB market WebSocket. Meant to run unattended for hours/
// days on a fixed host to build a baseline correction factor for downstream
// insider-trading timing analysis -- see README.md.
//
// NOTE: verify --match-mode against a real payload dump (--dump-raw-messages)
// before trusting any lag numbers from a live run. See config.hpp and the
// README for why tx-hash matching and fuzzy log-scan matching are kept
// strictly separate, opt-in choices rather than a single "best guess" mode.

#include <ixwebsocket/IXNetSystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <curl/curl.h>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "config.hpp"
#include "logging.hpp"
#include "ntp_check.hpp"
#include "resolve.hpp"
#include "rpc_client.hpp"
#include "storage.hpp"
#include "trade_event.hpp"
#include "ws_listener.hpp"

namespace {

// --- global shutdown signaling ------------------------------------------
std::atomic<bool> g_stopRequested{false};
std::mutex g_stopMutex;
std::condition_variable g_stopCv;

void handleSignal(int) {
    g_stopRequested = true;
    g_stopCv.notify_all();
}

void waitForStop() {
    std::unique_lock<std::mutex> lock(g_stopMutex);
    g_stopCv.wait(lock, [] { return g_stopRequested.load(); });
}

// --- bounded, thread-safe trade queue -----------------------------------
// Bounded so a slow RPC endpoint can't grow this without limit over a
// multi-day run; overflow drops the oldest queued item (logged) rather than
// blocking the WS callback thread, which must stay fast to keep up with
// reconnects/pings.
class TradeQueue {
public:
    explicit TradeQueue(size_t maxSize) : maxSize_(maxSize) {}

    void push(TradeEvent trade) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.size() >= maxSize_) {
            queue_.pop_front();
            ++dropped_;
        }
        queue_.push_back(std::move(trade));
        cv_.notify_one();
    }

    // Returns false if woken due to shutdown with nothing left to process.
    bool pop(TradeEvent* out) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [&] { return !queue_.empty() || g_stopRequested.load(); });
        if (queue_.empty()) return false;
        *out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void wakeAll() { cv_.notify_all(); }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    uint64_t dropped() const { return dropped_.load(); }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<TradeEvent> queue_;
    size_t maxSize_;
    std::atomic<uint64_t> dropped_{0};
};

// --- running lag stats (approximate, streaming) -------------------------
// True min/max are exact over the whole run. Median is approximated from a
// capped rolling window (most recent kMaxSamples) rather than keeping every
// sample forever, since this is meant to run for days.
class RunningLagStats {
public:
    void add(double lagMs) {
        std::lock_guard<std::mutex> lock(mtx_);
        ++count_;
        min_ = count_ == 1 ? lagMs : std::min(min_, lagMs);
        max_ = count_ == 1 ? lagMs : std::max(max_, lagMs);
        sum_ += lagMs;
        window_.push_back(lagMs);
        if (window_.size() > kMaxSamples) window_.pop_front();
    }

    struct Snapshot {
        uint64_t count = 0;
        double min = 0, mean = 0, median = 0, max = 0;
    };

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mtx_);
        Snapshot s;
        s.count = count_;
        if (count_ == 0) return s;
        s.min = min_;
        s.max = max_;
        s.mean = sum_ / static_cast<double>(count_);
        std::vector<double> sorted(window_.begin(), window_.end());
        std::sort(sorted.begin(), sorted.end());
        s.median = sorted[sorted.size() / 2];
        return s;
    }

private:
    static constexpr size_t kMaxSamples = 50000;
    mutable std::mutex mtx_;
    uint64_t count_ = 0;
    double min_ = 0, max_ = 0, sum_ = 0;
    std::deque<double> window_;
};

// --- NTP gate -------------------------------------------------------------
// This tool's entire purpose is measuring a real lag distribution; an
// unsynced local clock is indistinguishable from real ingestion lag in the
// output, so this refuses to proceed by default rather than silently
// producing numbers nobody should trust. --force-unsynced-clock overrides.
bool ntpGateOrExit(const AppConfig& cfg) {
    NtpCheckResult ntp = checkNtpSync();
    switch (ntp.status) {
        case NtpSyncStatus::Synced:
            logging::info("NTP sync check: OK");
            return true;
        case NtpSyncStatus::NotSynced:
            logging::error("NTP sync check: system clock is NOT synced. Every lag number this "
                            "tool produces will be corrupted by your clock offset until this is "
                            "fixed. Detail:\n" + ntp.detail);
            if (!cfg.force_unsynced_clock) {
                logging::error("refusing to start (pass --force-unsynced-clock to override, not recommended)");
                return false;
            }
            logging::warn("--force-unsynced-clock set, proceeding anyway");
            return true;
        case NtpSyncStatus::Unknown:
            logging::warn("NTP sync check: could not determine sync status. Detail:\n" + ntp.detail);
            logging::warn("proceeding, but treat lag numbers from this run as unverified until "
                          "you confirm clock sync manually.");
            return true;
    }
    return false;
}

// --- raw payload dump mode ------------------------------------------------
int runDumpRawMessages(const AppConfig& cfg) {
    logging::info("dump-raw-messages mode: writing " + std::to_string(cfg.dump_raw_messages) +
                  " raw payload(s) to " + cfg.dump_raw_path + ", then exiting. No RPC calls, no CSV output.");

    std::ofstream out(cfg.dump_raw_path, std::ios::out | std::ios::app);
    if (!out) {
        logging::error("could not open " + cfg.dump_raw_path + " for writing");
        return 1;
    }

    std::atomic<int> remaining{cfg.dump_raw_messages};
    WsListener ws(cfg.ws_url, cfg.asset_ids, cfg.reconnect_min_backoff_ms,
                  cfg.reconnect_max_backoff_ms, cfg.ping_interval_sec);
    std::mutex fileMtx;
    ws.setRawCallback([&](const std::string& raw) {
        {
            std::lock_guard<std::mutex> lock(fileMtx);
            if (remaining.load() <= 0) return;
            out << raw << "\n";
            out.flush();
        }
        if (--remaining <= 0) {
            g_stopRequested = true;
            g_stopCv.notify_all();
        }
    });
    ws.start();
    waitForStop();
    ws.stop();
    logging::info("wrote raw payloads to " + cfg.dump_raw_path + " -- inspect it to confirm whether "
                  "trades carry a transaction hash, then set --match-mode accordingly.");
    return 0;
}

void logStatsSnapshot(const WsListener& ws, const TradeQueue& queue, const RunningLagStats& stats) {
    auto snap = stats.snapshot();
    logging::info(
        "health: connected=" + std::string(ws.isConnected() ? "yes" : "no") +
        " connects=" + std::to_string(ws.connectCount()) +
        " disconnects=" + std::to_string(ws.disconnectCount()) +
        " messages=" + std::to_string(ws.messagesReceived()) +
        " queue_depth=" + std::to_string(queue.size()) +
        " queue_dropped=" + std::to_string(queue.dropped()) +
        " | resolved_lag_samples=" + std::to_string(snap.count) +
        (snap.count > 0
             ? " min_ms=" + std::to_string(snap.min) +
               " median_ms=" + std::to_string(snap.median) +
               " mean_ms=" + std::to_string(snap.mean) +
               " max_ms=" + std::to_string(snap.max)
             : ""));
}

} // namespace

int main(int argc, char** argv) {
    AppConfig cfg;
    if (!loadConfig(argc, argv, &cfg)) return 1;

    if (!cfg.log_file.empty()) logging::setLogFile(cfg.log_file);

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    ix::initNetSystem();

    if (cfg.dump_raw_messages > 0) {
        int rc = runDumpRawMessages(cfg);
        ix::uninitNetSystem();
        curl_global_cleanup();
        return rc;
    }

    if (!ntpGateOrExit(cfg)) {
        ix::uninitNetSystem();
        curl_global_cleanup();
        return 1;
    }

    logging::info("poly_lag_tracker starting: ws_url=" + cfg.ws_url +
                  " assets=" + std::to_string(cfg.asset_ids.size()) +
                  " match_mode=" + matchModeToString(cfg.match_mode) +
                  " output=" + cfg.output_path);
    if (cfg.match_mode == MatchMode::Auto &&
        (cfg.exchange_contract_address.empty() || cfg.order_filled_topic0.empty())) {
        logging::warn("match_mode=auto but fuzzy match isn't configured (exchange_contract_address/"
                      "order_filled_topic0 unset) -- trades without a tx hash in the WS payload will "
                      "be recorded as unmatched. Run with --dump-raw-messages first if you haven't "
                      "confirmed whether the real payload includes a tx hash.");
    }

    CsvStorage storage(cfg.output_path);
    TradeQueue queue(static_cast<size_t>(cfg.queue_max_size));
    RunningLagStats lagStats;

    WsListener ws(cfg.ws_url, cfg.asset_ids, cfg.reconnect_min_backoff_ms,
                  cfg.reconnect_max_backoff_ms, cfg.ping_interval_sec);
    ws.setTradeCallback([&queue](TradeEvent trade) { queue.push(std::move(trade)); });

    std::vector<std::thread> workers;
    workers.reserve(cfg.worker_threads);
    for (int i = 0; i < cfg.worker_threads; ++i) {
        workers.emplace_back([&] {
            PolygonRpcClient rpc(cfg.rpc_url, cfg.rpc_timeout_ms);
            for (;;) {
                TradeEvent trade;
                if (!queue.pop(&trade)) {
                    if (g_stopRequested.load()) break;
                    continue;
                }
                OnChainResolution resolution = resolveTrade(rpc, trade, cfg, &g_stopRequested);
                if (resolution.resolved) {
                    int64_t wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          trade.recv_wall.time_since_epoch()).count();
                    double lagMs = static_cast<double>(wallMs) -
                                    static_cast<double>(resolution.block_timestamp_unix) * 1000.0;
                    lagStats.add(lagMs);
                }
                storage.appendRow(OutputRow{trade, resolution});
            }
        });
    }

    std::thread statsThread([&] {
        std::unique_lock<std::mutex> lock(g_stopMutex);
        while (!g_stopRequested.load()) {
            g_stopCv.wait_for(lock, std::chrono::seconds(cfg.log_interval_sec),
                               [] { return g_stopRequested.load(); });
            if (g_stopRequested.load()) break;
            lock.unlock();
            logStatsSnapshot(ws, queue, lagStats);
            lock.lock();
        }
    });

    ws.start();
    logging::info("collection running. SIGINT/SIGTERM for graceful shutdown.");

    waitForStop();

    logging::info("shutdown requested, stopping WS and draining workers...");
    ws.stop();
    queue.wakeAll();
    for (auto& t : workers) t.join();
    statsThread.join();

    logStatsSnapshot(ws, queue, lagStats);
    logging::info("shutdown complete.");

    ix::uninitNetSystem();
    curl_global_cleanup();
    return 0;
}
