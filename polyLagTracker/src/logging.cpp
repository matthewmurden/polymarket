#include "logging.hpp"

#include <cstdio>
#include <ctime>
#include <mutex>

namespace logging {
namespace {

std::mutex g_mutex;
FILE* g_logFile = nullptr;

void writeLine(FILE* stream, const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string ts = isoTimestampUtc();
    std::fprintf(stream, "[%s] [%s] %s\n", ts.c_str(), level, msg.c_str());
    std::fflush(stream);
    if (g_logFile) {
        std::fprintf(g_logFile, "[%s] [%s] %s\n", ts.c_str(), level, msg.c_str());
        std::fflush(g_logFile);
    }
}

} // namespace

std::string isoTimestampUtc() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

void setLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
    if (path.empty()) return;
    g_logFile = std::fopen(path.c_str(), "a");
    if (!g_logFile) {
        std::fprintf(stderr, "warning: could not open log file %s for append\n", path.c_str());
    }
}

void info(const std::string& msg) { writeLine(stdout, "INFO", msg); }
void warn(const std::string& msg) { writeLine(stderr, "WARN", msg); }
void error(const std::string& msg) { writeLine(stderr, "ERROR", msg); }

} // namespace logging
