#include "wallet_history_store.hpp"

#include <cmath>
#include <ctime>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "logging.hpp"

using json = nlohmann::json;

double WalletHistoryRecord::sizeVariance() const {
    return trade_count > 1 ? size_m2 / static_cast<double>(trade_count - 1) : 0.0;
}

double WalletHistoryRecord::sizeStddev() const {
    return std::sqrt(sizeVariance());
}

void foldTradeIntoHistory(WalletHistoryRecord* rec, double size, const std::string& category, uint64_t tradeUnix) {
    ++rec->trade_count;

    if (rec->first_seen_unix == 0 || (tradeUnix != 0 && tradeUnix < rec->first_seen_unix)) {
        rec->first_seen_unix = tradeUnix;
    }

    // Welford's online algorithm: exact running mean/M2 in one pass, no
    // stored history of individual sizes needed.
    double delta = size - rec->size_mean;
    rec->size_mean += delta / static_cast<double>(rec->trade_count);
    double delta2 = size - rec->size_mean;
    rec->size_m2 += delta * delta2;

    ++rec->category_counts[category];
}

namespace {

void execOrDie(sqlite3* db, const char* sql) {
    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        logging::error(std::string("wallet_history_store: schema init failed: ") +
                       (errMsg ? errMsg : "unknown error"));
        sqlite3_free(errMsg);
    }
}

} // namespace

WalletHistoryStore::WalletHistoryStore(const std::string& dbPath) {
    if (sqlite3_open(dbPath.c_str(), &db_) != SQLITE_OK) {
        logging::error("wallet_history_store: failed to open " + dbPath + ": " +
                       (db_ ? sqlite3_errmsg(db_) : "unknown error"));
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return;
    }

    execOrDie(db_,
        "CREATE TABLE IF NOT EXISTS wallets ("
        "  wallet_address TEXT PRIMARY KEY,"
        "  had_zero_history_on_first_lookup INTEGER NOT NULL DEFAULT 0,"
        "  trade_count INTEGER NOT NULL DEFAULT 0,"
        "  first_seen_unix INTEGER NOT NULL DEFAULT 0,"
        "  size_mean REAL NOT NULL DEFAULT 0,"
        "  size_m2 REAL NOT NULL DEFAULT 0,"
        "  category_counts_json TEXT NOT NULL DEFAULT '{}',"
        "  last_updated_unix INTEGER NOT NULL DEFAULT 0"
        ");");

    execOrDie(db_,
        "CREATE TABLE IF NOT EXISTS market_categories ("
        "  condition_id TEXT PRIMARY KEY,"
        "  category TEXT NOT NULL,"
        "  looked_up_unix INTEGER NOT NULL DEFAULT 0"
        ");");
}

WalletHistoryStore::~WalletHistoryStore() {
    if (db_) sqlite3_close(db_);
}

WalletHistoryRecord WalletHistoryStore::get(const std::string& walletAddress) {
    WalletHistoryRecord rec;
    rec.wallet_address = walletAddress;

    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return rec;

    static const char* kSql =
        "SELECT had_zero_history_on_first_lookup, trade_count, first_seen_unix, "
        "size_mean, size_m2, category_counts_json, last_updated_unix "
        "FROM wallets WHERE wallet_address = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        logging::error(std::string("wallet_history_store: get() prepare failed: ") + sqlite3_errmsg(db_));
        return rec;
    }
    sqlite3_bind_text(stmt, 1, walletAddress.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        rec.known = true;
        rec.had_zero_history_on_first_lookup = sqlite3_column_int(stmt, 0) != 0;
        rec.trade_count = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
        rec.first_seen_unix = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
        rec.size_mean = sqlite3_column_double(stmt, 3);
        rec.size_m2 = sqlite3_column_double(stmt, 4);
        const unsigned char* catJsonText = sqlite3_column_text(stmt, 5);
        rec.last_updated_unix = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));

        if (catJsonText) {
            try {
                json j = json::parse(reinterpret_cast<const char*>(catJsonText));
                for (auto it = j.begin(); it != j.end(); ++it) {
                    rec.category_counts[it.key()] = it.value().get<uint64_t>();
                }
            } catch (const json::exception& e) {
                logging::warn("wallet_history_store: corrupt category_counts_json for " + walletAddress +
                              ": " + e.what());
            }
        }
    }

    sqlite3_finalize(stmt);
    return rec;
}

void WalletHistoryStore::upsert(const WalletHistoryRecord& rec) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return;

    json catJson = json::object();
    for (const auto& kv : rec.category_counts) catJson[kv.first] = kv.second;
    std::string catJsonText = catJson.dump();

    static const char* kSql =
        "INSERT INTO wallets (wallet_address, had_zero_history_on_first_lookup, trade_count, "
        "first_seen_unix, size_mean, size_m2, category_counts_json, last_updated_unix) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(wallet_address) DO UPDATE SET "
        "had_zero_history_on_first_lookup = excluded.had_zero_history_on_first_lookup, "
        "trade_count = excluded.trade_count, "
        "first_seen_unix = excluded.first_seen_unix, "
        "size_mean = excluded.size_mean, "
        "size_m2 = excluded.size_m2, "
        "category_counts_json = excluded.category_counts_json, "
        "last_updated_unix = excluded.last_updated_unix;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        logging::error(std::string("wallet_history_store: upsert() prepare failed: ") + sqlite3_errmsg(db_));
        return;
    }

    sqlite3_bind_text(stmt, 1, rec.wallet_address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, rec.had_zero_history_on_first_lookup ? 1 : 0);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(rec.trade_count));
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(rec.first_seen_unix));
    sqlite3_bind_double(stmt, 5, rec.size_mean);
    sqlite3_bind_double(stmt, 6, rec.size_m2);
    sqlite3_bind_text(stmt, 7, catJsonText.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, static_cast<sqlite3_int64>(rec.last_updated_unix));

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        logging::error(std::string("wallet_history_store: upsert() failed: ") + sqlite3_errmsg(db_));
    }
    sqlite3_finalize(stmt);
}

std::optional<std::string> WalletHistoryStore::getCachedCategory(const std::string& conditionId) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return std::nullopt;

    static const char* kSql = "SELECT category FROM market_categories WHERE condition_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        logging::error(std::string("wallet_history_store: getCachedCategory() prepare failed: ") +
                       sqlite3_errmsg(db_));
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, conditionId.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        result = text ? std::string(reinterpret_cast<const char*>(text)) : std::string();
    }
    sqlite3_finalize(stmt);
    return result;
}

void WalletHistoryStore::cacheCategory(const std::string& conditionId, const std::string& category) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return;

    static const char* kSql =
        "INSERT INTO market_categories (condition_id, category, looked_up_unix) VALUES (?, ?, ?) "
        "ON CONFLICT(condition_id) DO UPDATE SET category = excluded.category, "
        "looked_up_unix = excluded.looked_up_unix;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        logging::error(std::string("wallet_history_store: cacheCategory() prepare failed: ") +
                       sqlite3_errmsg(db_));
        return;
    }
    sqlite3_bind_text(stmt, 1, conditionId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(time(nullptr)));

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        logging::error(std::string("wallet_history_store: cacheCategory() failed: ") + sqlite3_errmsg(db_));
    }
    sqlite3_finalize(stmt);
}
