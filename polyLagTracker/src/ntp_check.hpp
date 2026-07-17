#pragma once

#include <string>

enum class NtpSyncStatus {
    Synced,
    NotSynced,
    Unknown, // couldn't determine (chronyc/ntpstat unavailable) -- treat with suspicion, not as a green light
};

struct NtpCheckResult {
    NtpSyncStatus status = NtpSyncStatus::Unknown;
    std::string detail; // raw tool output / explanation, for the log
};

// IMPORTANT: every lag number this tool produces is only meaningful if the
// local system clock is actually NTP-synced -- an offset clock shows up
// indistinguishable from real ingestion lag. This check shells out to
// `chronyc tracking` (preferred, gives a leap-status + offset) and falls
// back to `ntpstat` if chrony isn't installed. Resolve any NotSynced/Unknown
// result (e.g. `sudo systemctl enable --now chronyd` / `sudo chronyc
// makestep`) before trusting any lag numbers from this tool.
NtpCheckResult checkNtpSync();
