#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

#include "resolve.hpp"
#include "trade_event.hpp"

// One fully-processed row: a trade event plus whatever on-chain resolution
// (successful or not) was found for it.
struct OutputRow {
    TradeEvent trade;
    OnChainResolution resolution;
};

// Appends rows to a CSV file, one flush per row so a crash loses at most
// the row currently being written, not the whole run. Not a general
// storage abstraction -- SQLite was considered but CSV keeps this
// dependency-free and trivially reprocessable with pandas, matching how
// polyAreaTesting's own CSV output is used.
class CsvStorage {
public:
    explicit CsvStorage(const std::string& path);

    void appendRow(const OutputRow& row);

private:
    std::mutex mtx_;
    std::ofstream out_;
};
