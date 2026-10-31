#include "edgegate/log.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>

#include "edgegate/util.hpp"

namespace eg {
namespace {
std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_log_mutex;

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}
}  // namespace

void set_log_level(LogLevel level) { g_level.store(level, std::memory_order_relaxed); }
LogLevel current_log_level() { return g_level.load(std::memory_order_relaxed); }

bool parse_log_level(std::string_view text, LogLevel& out) {
    if (iequals(text, "debug")) { out = LogLevel::Debug; return true; }
    if (iequals(text, "info")) { out = LogLevel::Info; return true; }
    if (iequals(text, "warn") || iequals(text, "warning")) { out = LogLevel::Warn; return true; }
    if (iequals(text, "error")) { out = LogLevel::Error; return true; }
    return false;
}

bool log_enabled(LogLevel level) {
    return static_cast<int>(level) >= static_cast<int>(g_level.load(std::memory_order_relaxed));
}

void log_line(LogLevel level, std::string_view message) {
    if (!log_enabled(level)) return;
    const std::string line = iso8601_now() + " [" + level_name(level) + "] " +
                             std::string(message) + "\n";
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

}  // namespace eg
