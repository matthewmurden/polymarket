#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "trade_event.hpp"

// One persistent connection to Polymarket's CLOB market WS channel.
//
// Unlike polyAreaTesting's ws_bench (which does a fresh connect/disconnect
// per sample to measure handshake cost), this holds a single long-lived
// ix::WebSocket and relies on IXWebSocket's built-in automatic reconnection
// (exponential backoff between setMinRetryWaitTime/setMaxRetryWaitTime) to
// recover from drops. Every Open/Close/Error is logged with a timestamp.
class WsListener {
public:
    using TradeCallback = std::function<void(TradeEvent)>;
    using RawCallback = std::function<void(const std::string&)>;

    WsListener(std::string url, std::vector<std::string> assetIds,
               int minBackoffMs, int maxBackoffMs, int pingIntervalSec);
    ~WsListener();

    // Called for every message that parses as a trade-shaped payload (see
    // parseTradeEvent). Invoked on the IXWebSocket callback thread, so it
    // must be fast/non-blocking -- hand off to a queue, don't do RPC here.
    void setTradeCallback(TradeCallback cb);

    // Called for every raw message payload, trade-shaped or not. Used by
    // --dump-raw-messages to capture real payloads for schema inspection.
    void setRawCallback(RawCallback cb);

    void start();
    void stop();

    uint64_t connectCount() const { return connectCount_.load(); }
    uint64_t disconnectCount() const { return disconnectCount_.load(); }
    uint64_t messagesReceived() const { return messagesReceived_.load(); }
    bool isConnected() const { return connected_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string url_;
    std::vector<std::string> assetIds_;
    int minBackoffMs_;
    int maxBackoffMs_;
    int pingIntervalSec_;

    TradeCallback tradeCb_;
    RawCallback rawCb_;

    std::atomic<uint64_t> connectCount_{0};
    std::atomic<uint64_t> disconnectCount_{0};
    std::atomic<uint64_t> messagesReceived_{0};
    std::atomic<bool> connected_{false};

    std::string buildSubscribeMessage() const;
};
