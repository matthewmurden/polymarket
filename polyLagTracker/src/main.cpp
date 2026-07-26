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
#include <unordered_set>
#include <vector>

#include "config.hpp"
#include "discovery.hpp"
#include "logging.hpp"
#include "ntp_check.hpp"
#include "resolve.hpp"
#include "rpc_client.hpp"
#include "storage.hpp"
#include "trade_event.hpp"
#include "wallet_resolve.hpp"
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

// --- asset auto-discovery -------------------------------------------------
// Fixed timeout for discovery HTTP calls -- not exposed as a flag since the
// spec calls for sample-size/top-n/refresh-interval knobs, not a separate
// timeout; matches the RPC client's default order of magnitude.
constexpr long kDiscoveryTimeoutMs = 8000;

// Runs the initial discovery call and populates cfg->asset_ids from it.
// Logs exactly which asset ids (and their trade counts in the sample) were
// selected, so a run is reproducible/debuggable after the fact. Returns
// false if discovery failed outright -- callers should exit non-zero
// rather than silently falling back to an empty subscription list.
bool runInitialDiscoveryOrExit(AppConfig* cfg) {
    logging::info("auto-discover-assets: sampling " + std::to_string(cfg->discover_trade_sample_size) +
                  " recent trades to pick the top " + std::to_string(cfg->discover_top_n) +
                  " currently active asset(s)...");
    auto discovered = discoverActiveAssets(cfg->discover_trade_sample_size, cfg->discover_top_n,
                                            kDiscoveryTimeoutMs);
    if (!discovered) {
        logging::error("auto-discover-assets: could not determine an active asset list (see "
                       "discovery errors above). Pass --asset-ids explicitly instead if this "
                       "keeps happening.");
        return false;
    }

    cfg->asset_ids.clear();
    std::string summary;
    for (const auto& d : *discovered) {
        cfg->asset_ids.push_back(d.asset_id);
        summary += "\n  " + d.asset_id + " (" + std::to_string(d.trade_count) + " trades in sample)";
    }
    logging::info("auto-discover-assets: selected " + std::to_string(cfg->asset_ids.size()) +
                  " asset id(s):" + summary);
    return true;
}

// Periodic re-discovery for long runs: markets like the 5-minute BTC/ETH/
// XRP up/down markets roll over every few minutes, so a snapshot taken only
// at startup goes stale well before a multi-day run ends. Diffs the newly
// discovered top-N set against what's currently subscribed, logs every
// add/drop with its reason, and reconnects (via WsListener::resubscribe)
// only if the set actually changed. Failures here are logged and skipped
// rather than fatal -- unlike the startup call, an unattended run shouldn't
// die over one transient HTTP hiccup hours in; it just tries again next
// interval, keeping the current subscription in the meantime.
void refreshDiscoveredAssets(const AppConfig& cfg, WsListener* ws) {
    auto discovered = discoverActiveAssets(cfg.discover_trade_sample_size, cfg.discover_top_n,
                                            kDiscoveryTimeoutMs);
    if (!discovered) {
        logging::error("discovery refresh failed; keeping the current asset subscription "
                       "unchanged (will retry at the next refresh interval)");
        return;
    }

    std::vector<std::string> newIds;
    newIds.reserve(discovered->size());
    for (const auto& d : *discovered) newIds.push_back(d.asset_id);

    std::vector<std::string> oldIds = ws->currentAssetIds();
    std::unordered_set<std::string> oldSet(oldIds.begin(), oldIds.end());
    std::unordered_set<std::string> newSet(newIds.begin(), newIds.end());

    std::vector<std::string> added, dropped;
    for (const auto& id : newIds) if (!oldSet.count(id)) added.push_back(id);
    for (const auto& id : oldIds) if (!newSet.count(id)) dropped.push_back(id);

    if (added.empty() && dropped.empty()) {
        logging::info("discovery refresh: no change to the top-" + std::to_string(cfg.discover_top_n) +
                      " asset set");
        return;
    }

    for (const auto& id : added) logging::info("discovery refresh: adding asset " + id + " (newly active)");
    for (const auto& id : dropped)
        logging::info("discovery refresh: dropping asset " + id + " (aged out of top-" +
                      std::to_string(cfg.discover_top_n) + ")");

    ws->resubscribe(newIds);
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

void logStatsSnapshot(const WsListener& ws, const TradeQueue& queue, const RunningLagStats& stats,
                      uint64_t walletResolved, uint64_t walletUnresolved) {
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
             : "") +
        " | wallet_resolved=" + std::to_string(walletResolved) +
        " wallet_unresolved=" + std::to_string(walletUnresolved));
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

    if (cfg.auto_discover_assets) {
        if (!runInitialDiscoveryOrExit(&cfg)) {
            ix::uninitNetSystem();
            curl_global_cleanup();
            return 1;
        }
    }

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
    std::atomic<uint64_t> walletResolvedCount{0};
    std::atomic<uint64_t> walletUnresolvedCount{0};

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

                // Wallet resolution now happens inside resolveTrade() itself
                // -- decoded straight from the same on-chain receipt/logs
                // already fetched for block-timestamp confirmation, not as
                // a separate delayed Data-API retry loop. See
                // wallet_resolve.hpp/resolve.cpp.
                OnChainResolution resolution = resolveTrade(rpc, trade, cfg, &g_stopRequested);
                if (resolution.wallet.resolved) ++walletResolvedCount; else ++walletUnresolvedCount;

                if (resolution.resolved) {
                    int64_t wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          trade.recv_wall.time_since_epoch()).count();
                    double lagMs = static_cast<double>(wallMs) -
                                    static_cast<double>(resolution.block_timestamp_unix) * 1000.0;
                    lagStats.add(lagMs);
                }
                storage.appendRow(OutputRow{trade, resolution, resolution.wallet});
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
            logStatsSnapshot(ws, queue, lagStats, walletResolvedCount.load(), walletUnresolvedCount.load());
            lock.lock();
        }
    });

    // Only re-discovers on an interval when the asset list came from
    // --auto-discover-assets in the first place -- a manually-specified
    // --asset-ids list stays static for the whole run, per spec.
    std::thread discoveryThread;
    if (cfg.auto_discover_assets && cfg.discover_refresh_interval_sec > 0) {
        discoveryThread = std::thread([&] {
            std::unique_lock<std::mutex> lock(g_stopMutex);
            while (!g_stopRequested.load()) {
                g_stopCv.wait_for(lock, std::chrono::seconds(cfg.discover_refresh_interval_sec),
                                   [] { return g_stopRequested.load(); });
                if (g_stopRequested.load()) break;
                lock.unlock();
                refreshDiscoveredAssets(cfg, &ws);
                lock.lock();
            }
        });
    }

    ws.start();
    logging::info("collection running. SIGINT/SIGTERM for graceful shutdown.");

    waitForStop();

    logging::info("shutdown requested, stopping WS and draining workers...");
    ws.stop();
    queue.wakeAll();
    for (auto& t : workers) t.join();
    statsThread.join();
    if (discoveryThread.joinable()) discoveryThread.join();

    logStatsSnapshot(ws, queue, lagStats, walletResolvedCount.load(), walletUnresolvedCount.load());
    logging::info("shutdown complete.");

    ix::uninitNetSystem();
    curl_global_cleanup();
    return 0;
}
