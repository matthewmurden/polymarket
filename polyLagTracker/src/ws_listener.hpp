#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
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
//
// The subscribed asset set can only be chosen at connection-establishment
// time: confirmed empirically that sending a second subscribe message on
// an already-open connection gets rejected by Polymarket's server with a
// plain-text "INVALID OPERATION" reply, even if the message is identical
// to the first. There is no live add/remove. resubscribe() below changes
// the watched set by closing and reopening the connection.
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

    // Closes the current connection (if any) and opens a new one
    // subscribed to newAssetIds -- see class comment for why this can't be
    // done as a live update on the existing connection. Intended to be
    // called from a single dedicated refresh thread, not concurrently with
    // itself. Safe to call after start(); logs a summary line itself, but
    // per-asset added/dropped reasons are expected to be logged by the
    // caller (which has the old/new sets to diff) before calling this.
    void resubscribe(std::vector<std::string> newAssetIds);

    std::vector<std::string> currentAssetIds() const;

    uint64_t connectCount() const { return connectCount_.load(); }
    uint64_t disconnectCount() const { return disconnectCount_.load(); }
    uint64_t messagesReceived() const { return messagesReceived_.load(); }
    bool isConnected() const { return connected_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string url_;
    mutable std::mutex assetIdsMtx_;
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
