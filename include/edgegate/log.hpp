#pragma once

#include <sstream>
#include <string>
#include <string_view>

namespace eg {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_log_level(LogLevel level);
LogLevel current_log_level();
bool parse_log_level(std::string_view text, LogLevel& out);
bool log_enabled(LogLevel level);
void log_line(LogLevel level, std::string_view message);

template <typename... Args>
void log_msg(LogLevel level, Args&&... args) {
    if (!log_enabled(level)) return;
    std::ostringstream os;
    (os << ... << args);
    log_line(level, os.str());
}

}  // namespace eg
