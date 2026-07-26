#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

#include "resolve.hpp"
#include "trade_event.hpp"
#include "wallet_resolve.hpp"

// One fully-processed row: a trade event, whatever on-chain resolution
// (successful or not) was found for it, and whatever wallet-address lookup
// (successful or not) was found for it.
struct OutputRow {
    TradeEvent trade;
    OnChainResolution resolution;
    WalletResolution wallet;
};

// Appends rows to a CSV file, one flush per row so a crash loses at most
// the row currently being written, not the whole run. Not a general
// storage abstraction -- SQLite was considered but CSV keeps this
// dependency-free and trivially reprocessable with pandas, matching how
// polyAreaTesting's own CSV output is used.
//
// Wallet columns (wallet_address, wallet_resolved, wallet_resolve_note)
// were appended at the end of an already-existing schema, keeping every
// prior column unchanged and in position. If you point this at a CSV that
// already has rows written before this feature existed, the header (now
// describing 3 extra columns) will describe those older rows
// inconsistently -- start a fresh --output file when adopting wallet
// lookup rather than resuming into an old one, or reprocess the header
// mismatch yourself downstream.
class CsvStorage {
public:
    explicit CsvStorage(const std::string& path);

    void appendRow(const OutputRow& row);

private:
    std::mutex mtx_;
    std::ofstream out_;
};
