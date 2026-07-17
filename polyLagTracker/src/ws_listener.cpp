#include "ws_listener.hpp"

#include <ixwebsocket/IXWebSocket.h>

#include <chrono>
#include <sstream>

#include "logging.hpp"

struct WsListener::Impl {
    ix::WebSocket ws;
};

WsListener::WsListener(std::string url, std::vector<std::string> assetIds,
                        int minBackoffMs, int maxBackoffMs, int pingIntervalSec)
    : impl_(std::make_unique<Impl>()),
      url_(std::move(url)),
      assetIds_(std::move(assetIds)),
      minBackoffMs_(minBackoffMs),
      maxBackoffMs_(maxBackoffMs),
      pingIntervalSec_(pingIntervalSec) {}

WsListener::~WsListener() {
    stop();
}

void WsListener::setTradeCallback(TradeCallback cb) { tradeCb_ = std::move(cb); }
void WsListener::setRawCallback(RawCallback cb) { rawCb_ = std::move(cb); }

std::string WsListener::buildSubscribeMessage() const {
    std::lock_guard<std::mutex> lock(assetIdsMtx_);
    std::ostringstream oss;
    oss << "{\"assets_ids\":[";
    for (size_t i = 0; i < assetIds_.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << assetIds_[i] << "\"";
    }
    oss << "],\"type\":\"market\"}";
    return oss.str();
}

std::vector<std::string> WsListener::currentAssetIds() const {
    std::lock_guard<std::mutex> lock(assetIdsMtx_);
    return assetIds_;
}

void WsListener::start() {
    ix::WebSocket& ws = impl_->ws;
    ws.setUrl(url_);
    // IXWebSocket auto-reconnects by default; these bounds give it
    // exponential backoff between minBackoffMs_ and maxBackoffMs_.
    ws.setMinWaitBetweenReconnectionRetries(minBackoffMs_);
    ws.setMaxWaitBetweenReconnectionRetries(maxBackoffMs_);
    ws.setPingInterval(pingIntervalSec_);

    const std::string subscribeMsg = buildSubscribeMessage();
    const size_t assetCount = currentAssetIds().size();

    ws.setOnMessageCallback([this, subscribeMsg, assetCount](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open: {
                connected_ = true;
                uint64_t n = ++connectCount_;
                logging::info("ws connected (connect #" + std::to_string(n) +
                              "), sending subscribe for " + std::to_string(assetCount) + " asset id(s)");
                impl_->ws.send(subscribeMsg);
                break;
            }
            case ix::WebSocketMessageType::Close: {
                connected_ = false;
                uint64_t n = ++disconnectCount_;
                logging::warn("ws disconnected (disconnect #" + std::to_string(n) +
                              "), code=" + std::to_string(msg->closeInfo.code) +
                              " reason=" + msg->closeInfo.reason +
                              " -- IXWebSocket will auto-reconnect with backoff");
                break;
            }
            case ix::WebSocketMessageType::Error: {
                logging::error("ws error: " + msg->errorInfo.reason);
                break;
            }
            case ix::WebSocketMessageType::Message: {
                // Timestamp immediately, before any parsing, so the lag
                // measurement reflects receipt time as closely as possible.
                auto recvMono = std::chrono::steady_clock::now();
                auto recvWall = std::chrono::system_clock::now();
                ++messagesReceived_;

                if (rawCb_) rawCb_(msg->str);

                // A single WS frame can carry a JSON array of several
                // objects (confirmed live: Polymarket batches one "book"
                // snapshot per subscribed asset into one frame on
                // subscribe) as well as a single bare object; handle both.
                for (const auto& obj : splitTopLevelMessage(msg->str)) {
                    TradeEvent trade;
                    trade.recv_mono = recvMono;
                    trade.recv_wall = recvWall;
                    if (parseTradeEvent(obj, &trade)) {
                        if (tradeCb_) tradeCb_(std::move(trade));
                    }
                }
                break;
            }
            default:
                break;
        }
    });

    ws.start();
}

void WsListener::stop() {
    impl_->ws.stop();
}

void WsListener::resubscribe(std::vector<std::string> newAssetIds) {
    logging::info("resubscribe: closing current connection to apply an updated asset list (" +
                  std::to_string(newAssetIds.size()) + " assets) -- Polymarket's market channel "
                  "rejects a second subscribe message on an already-open connection (confirmed "
                  "empirically: the server replies \"INVALID OPERATION\"), so there's no live "
                  "add/remove; a fresh connection is required to change the watched set.");
    // ix::WebSocket::stop() blocks until its background thread has fully
    // quiesced, so it's safe to mutate assetIds_ immediately afterward --
    // no message callback can still be in flight referencing the old list.
    impl_->ws.stop();
    {
        std::lock_guard<std::mutex> lock(assetIdsMtx_);
        assetIds_ = std::move(newAssetIds);
    }
    start();
}
