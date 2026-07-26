#include "storage.hpp"

#include <chrono>
#include <ctime>
#include <sys/stat.h>

#include "logging.hpp"

namespace {

std::string csvEscape(const std::string& field) {
    bool needsQuoting = field.find_first_of(",\"\n\r") != std::string::npos;
    if (!needsQuoting) return field;
    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

std::string matchMethodToString(MatchMethod m) {
    switch (m) {
        case MatchMethod::TxHash: return "tx_hash";
        case MatchMethod::FuzzyLogScan: return "fuzzy_log_scan";
        case MatchMethod::Unmatched: return "unmatched";
    }
    return "unknown";
}

bool fileExistsNonEmpty(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    return st.st_size > 0;
}

} // namespace

CsvStorage::CsvStorage(const std::string& path) {
    bool writeHeader = !fileExistsNonEmpty(path);
    out_.open(path, std::ios::out | std::ios::app);
    if (!out_) {
        logging::error("failed to open CSV output path: " + path);
        return;
    }
    if (writeHeader) {
        out_ << "recv_wall_iso,recv_wall_unix_ms,day_of_week,hour_of_day,"
                "market,asset_id,side,price,size,payload_timestamp,"
                "tx_hash,match_method,resolved,block_number,block_timestamp_unix,"
                "lag_ms,note,raw_json,"
                "wallet_address,wallet_resolved,wallet_resolve_note\n";
        out_.flush();
    }
}

void CsvStorage::appendRow(const OutputRow& row) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!out_) return;

    const auto& trade = row.trade;
    const auto& res = row.resolution;
    const auto& wallet = row.wallet;

    auto wallTimeT = std::chrono::system_clock::to_time_t(trade.recv_wall);
    auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      trade.recv_wall.time_since_epoch()).count();

    std::tm gmt{};
    gmtime_r(&wallTimeT, &gmt);
    char isoBuf[40];
    std::strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%dT%H:%M:%SZ", &gmt);

    static const char* kDayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    std::string dayOfWeek = kDayNames[gmt.tm_wday];
    int hourOfDay = gmt.tm_hour;

    std::string lagMsStr;
    if (res.resolved) {
        int64_t lagMs = wallMs - static_cast<int64_t>(res.block_timestamp_unix) * 1000;
        lagMsStr = std::to_string(lagMs);
    }

    out_ << isoBuf << ","
         << wallMs << ","
         << dayOfWeek << ","
         << hourOfDay << ","
         << csvEscape(trade.market) << ","
         << csvEscape(trade.asset_id) << ","
         << csvEscape(trade.side) << ","
         << csvEscape(trade.price) << ","
         << csvEscape(trade.size) << ","
         << csvEscape(trade.payload_timestamp) << ","
         << csvEscape(res.tx_hash.empty() ? trade.tx_hash : res.tx_hash) << ","
         << matchMethodToString(res.method) << ","
         << (res.resolved ? "true" : "false") << ","
         << (res.resolved ? std::to_string(res.block_number) : "") << ","
         << (res.resolved ? std::to_string(res.block_timestamp_unix) : "") << ","
         << lagMsStr << ","
         << csvEscape(res.note) << ","
         << csvEscape(trade.raw_json) << ","
         << csvEscape(wallet.wallet_address) << ","
         << (wallet.resolved ? "true" : "false") << ","
         << csvEscape(wallet.note) << "\n";
    out_.flush();
}
