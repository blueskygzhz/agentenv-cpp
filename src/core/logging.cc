// SPDX-License-Identifier: MIT
#include "agentenv/core/logging.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace agentenv {
namespace core {

namespace {
std::mutex g_out_mu;

const char* LevelName(LogLevel lv) {
    switch (lv) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}
}  // namespace

Logger& Logger::Get() {
    static Logger inst;
    return inst;
}

Logger::Logger() : level_(LogLevel::Info) {}
void Logger::SetLevel(LogLevel lv) { level_ = lv; }
LogLevel Logger::Level() const { return level_; }

void Logger::Log(LogLevel lv, const char* file, int line, const std::string& msg) {
    // ISO-8601-ish timestamp in local time (thread-safe path).
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[24];
    std::strftime(ts, sizeof(ts), "%FT%T", &tm);

    // Strip the leading path from file.
    const char* base = std::strrchr(file, '/');
    base = base ? base + 1 : file;

    std::lock_guard<std::mutex> lg(g_out_mu);
    std::fprintf(stderr, "%s %s %s:%d  %s\n", ts, LevelName(lv), base, line, msg.c_str());
}

}  // namespace core
}  // namespace agentenv
