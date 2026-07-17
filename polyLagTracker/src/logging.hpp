#pragma once

#include <string>

// Minimal thread-safe timestamped logger. Writes to stdout (info) / stderr
// (warn/error) and, if configured via setLogFile(), tees the same lines to
// a log file so `nohup ./poly_lag_tracker &` still leaves a durable record.
namespace logging {

void setLogFile(const std::string& path);

void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);

// Returns current UTC wall-clock time as "YYYY-MM-DDTHH:MM:SSZ".
std::string isoTimestampUtc();

} // namespace logging
