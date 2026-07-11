#pragma once

#include <string>

#include "stats.hpp"

struct WsEndpointResult {
    std::string name;
    std::string url;
    // Time from calling start() to the transport-level Open event: covers
    // TCP connect + TLS handshake + WS upgrade, i.e. the true cost of a
    // cold reconnect from this host.
    LatencyStats handshake_stats;
    // Time from sending the subscribe message to the first data frame
    // (Polymarket pushes a full `book` snapshot immediately on subscribe).
    LatencyStats first_message_stats;
};

// Runs `samples` independent connect/subscribe/disconnect cycles against
// the market channel, subscribing to `assetId`. Each sample is a fresh
// connection (no reuse) so handshake cost is measured every time, matching
// how a bot reconnecting after a drop would experience it.
WsEndpointResult benchWsEndpoint(const std::string& name, const std::string& url,
                                  const std::string& assetId,
                                  int samples, int warmup, int delay_ms,
                                  int connectTimeoutMs, int messageTimeoutMs);
