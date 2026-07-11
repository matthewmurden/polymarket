#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

// Latency stats over a set of successful samples, in milliseconds.
// `failures` and `attempts` are tracked separately so a bad run shows up
// as a failure count rather than silently skewing min/mean/etc.
struct LatencyStats {
    size_t attempts = 0;
    size_t failures = 0;
    double min_ms = 0;
    double mean_ms = 0;
    double median_ms = 0;
    double p95_ms = 0;
    double p99_ms = 0;
    double max_ms = 0;
};

inline double percentile(const std::vector<double>& sorted, double pct) {
    if (sorted.empty()) return 0.0;
    double idx = pct * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = std::min(lo + 1, sorted.size() - 1);
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

inline LatencyStats computeStats(std::vector<double> samples_ms, size_t attempts, size_t failures) {
    LatencyStats s;
    s.attempts = attempts;
    s.failures = failures;
    if (samples_ms.empty()) return s;
    std::sort(samples_ms.begin(), samples_ms.end());
    s.min_ms = samples_ms.front();
    s.max_ms = samples_ms.back();
    s.mean_ms = std::accumulate(samples_ms.begin(), samples_ms.end(), 0.0) / static_cast<double>(samples_ms.size());
    s.median_ms = percentile(samples_ms, 0.50);
    s.p95_ms = percentile(samples_ms, 0.95);
    s.p99_ms = percentile(samples_ms, 0.99);
    return s;
}
