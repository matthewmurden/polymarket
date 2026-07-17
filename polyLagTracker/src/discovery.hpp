#pragma once

#include <optional>
#include <string>
#include <vector>

// One asset id and how many times it showed up in the sampled trades --
// kept together (rather than just returning bare ids) so callers can log
// exactly what was selected and why, for reproducibility/debugging.
struct DiscoveredAsset {
    std::string asset_id;
    int trade_count = 0;
};

// Fetches the most recent `sampleSize` trades from Polymarket's public Data
// API (GET https://data-api.polymarket.com/trades?limit=sampleSize) and
// returns the `topN` most frequently-traded asset ids in that sample --
// trade frequency in a recent sample is used as a proxy for current
// activity, since Polymarket doesn't expose a direct "currently active
// markets" endpoint. Results are sorted by trade count, descending.
//
// Retries once (after a short fixed delay) on transport failure, a
// non-2xx response, unparseable JSON, or an empty trade list; returns
// nullopt if both attempts fail. Never returns an empty-but-successful
// result silently -- an empty asset list is treated as a failure so
// callers don't end up subscribed to nothing without knowing why.
std::optional<std::vector<DiscoveredAsset>> discoverActiveAssets(int sampleSize, int topN, long timeoutMs);
