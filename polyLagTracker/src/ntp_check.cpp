#include "ntp_check.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <sys/wait.h>

namespace {

// Runs `cmd`, capturing stdout. Returns {exitCode, output}. exitCode is -1
// if the shell itself couldn't be spawned.
std::pair<int, std::string> runCommand(const std::string& cmd) {
    std::array<char, 256> buf{};
    std::string output;
    // Redirect stderr into stdout so tool errors ("command not found") show
    // up in output too, and so popen's stream doesn't block on stderr.
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) return {-1, ""};
    while (fgets(buf.data(), buf.size(), pipe) != nullptr) {
        output += buf.data();
    }
    int status = pclose(pipe);
    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {exitCode, output};
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool tryChrony(NtpCheckResult* out) {
    auto [exitCode, output] = runCommand("chronyc tracking");
    if (exitCode != 0 || output.find("Leap status") == std::string::npos) return false;

    std::string lower = toLower(output);
    out->detail = "chronyc tracking:\n" + output;
    if (lower.find("leap status") != std::string::npos && lower.find("normal") != std::string::npos) {
        out->status = NtpSyncStatus::Synced;
    } else {
        out->status = NtpSyncStatus::NotSynced;
    }
    return true;
}

bool tryNtpstat(NtpCheckResult* out) {
    auto [exitCode, output] = runCommand("ntpstat");
    // ntpstat exit codes: 0 = synchronized, 1 = not synchronized, 2 = unknown.
    if (exitCode < 0) return false;
    out->detail = "ntpstat:\n" + output;
    if (exitCode == 0) out->status = NtpSyncStatus::Synced;
    else if (exitCode == 1) out->status = NtpSyncStatus::NotSynced;
    else out->status = NtpSyncStatus::Unknown;
    return true;
}

} // namespace

NtpCheckResult checkNtpSync() {
    NtpCheckResult result;
    if (tryChrony(&result)) return result;
    if (tryNtpstat(&result)) return result;

    result.status = NtpSyncStatus::Unknown;
    result.detail = "neither chronyc nor ntpstat is available on this host -- "
                     "clock sync could not be verified. Install chrony "
                     "(`apt install chrony`) and confirm `chronyc tracking` "
                     "shows 'Leap status : Normal' before trusting lag numbers.";
    return result;
}
