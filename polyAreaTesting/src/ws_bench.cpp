#include "ws_bench.hpp"

#include <ixwebsocket/IXWebSocket.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

using Clock = std::chrono::steady_clock;

namespace {

struct SampleSync {
    std::mutex mtx;
    std::condition_variable cv;
    bool open = false;
    bool gotMessage = false;
    bool errored = false;
    Clock::time_point tOpen;
    Clock::time_point tFirstMessage;
};

} // namespace

WsEndpointResult benchWsEndpoint(const std::string& name, const std::string& url,
                                  const std::string& assetId,
                                  int samples, int warmup, int delay_ms,
                                  int connectTimeoutMs, int messageTimeoutMs) {
    WsEndpointResult result;
    result.name = name;
    result.url = url;

    std::vector<double> handshakeSamples;
    std::vector<double> messageSamples;
    size_t handshakeAttempts = 0, handshakeFailures = 0;
    size_t messageAttempts = 0, messageFailures = 0;

    const std::string subscribeMsg =
        "{\"assets_ids\":[\"" + assetId + "\"],\"type\":\"market\"}";

    int totalRounds = warmup + samples;
    for (int i = 0; i < totalRounds; ++i) {
        bool isWarmup = i < warmup;
        auto sync = std::make_shared<SampleSync>();

        ix::WebSocket ws;
        ws.setUrl(url);
        int handshakeTimeoutSecs = (connectTimeoutMs + 999) / 1000;
        ws.setHandshakeTimeout(handshakeTimeoutSecs > 0 ? handshakeTimeoutSecs : 5);

        ws.setOnMessageCallback([sync](const ix::WebSocketMessagePtr& msg) {
            std::lock_guard<std::mutex> lock(sync->mtx);
            if (msg->type == ix::WebSocketMessageType::Open) {
                sync->open = true;
                sync->tOpen = Clock::now();
                sync->cv.notify_all();
            } else if (msg->type == ix::WebSocketMessageType::Message) {
                if (!sync->gotMessage) {
                    sync->gotMessage = true;
                    sync->tFirstMessage = Clock::now();
                    sync->cv.notify_all();
                }
            } else if (msg->type == ix::WebSocketMessageType::Error) {
                sync->errored = true;
                sync->cv.notify_all();
            }
        });

        Clock::time_point tStart = Clock::now();
        ws.start();

        bool gotOpen;
        {
            std::unique_lock<std::mutex> lock(sync->mtx);
            gotOpen = sync->cv.wait_for(lock, std::chrono::milliseconds(connectTimeoutMs),
                                         [&] { return sync->open || sync->errored; });
            gotOpen = gotOpen && sync->open;
        }

        if (!isWarmup) ++handshakeAttempts;
        if (gotOpen) {
            double ms = std::chrono::duration<double, std::milli>(sync->tOpen - tStart).count();
            if (!isWarmup) handshakeSamples.push_back(ms);
        } else {
            if (!isWarmup) {
                ++handshakeFailures;
                std::fprintf(stderr, "[%s] handshake failed/timed out (attempt %d/%d)\n",
                             name.c_str(), i - warmup + 1, samples);
            }
            ws.stop();
            if (i != totalRounds - 1) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            continue;
        }

        Clock::time_point tSubscribeSent = Clock::now();
        ws.send(subscribeMsg);

        bool gotMsg;
        {
            std::unique_lock<std::mutex> lock(sync->mtx);
            gotMsg = sync->cv.wait_for(lock, std::chrono::milliseconds(messageTimeoutMs),
                                        [&] { return sync->gotMessage || sync->errored; });
            gotMsg = gotMsg && sync->gotMessage;
        }

        if (!isWarmup) ++messageAttempts;
        if (gotMsg) {
            double ms = std::chrono::duration<double, std::milli>(sync->tFirstMessage - tSubscribeSent).count();
            if (!isWarmup) messageSamples.push_back(ms);
        } else if (!isWarmup) {
            ++messageFailures;
            std::fprintf(stderr, "[%s] no message received after subscribe (attempt %d/%d)\n",
                         name.c_str(), i - warmup + 1, samples);
        }

        ws.stop();

        if (i != totalRounds - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }

    result.handshake_stats = computeStats(handshakeSamples, handshakeAttempts, handshakeFailures);
    result.first_message_stats = computeStats(messageSamples, messageAttempts, messageFailures);
    return result;
}
