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

std::string FromUtf8Lossy(const std::string& bytes) {
    static const char kReplacement[] = "\xEF\xBF\xBD";
    std::string out;
    out.reserve(bytes.size());
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        // Expected length and the valid range of the second byte (Unicode
        // Table 3-7), which is what makes overlongs and surrogates invalid.
        std::size_t len = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c == 0xE0) {
            len = 3; lo = 0xA0;
        } else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) {
            len = 3;
        } else if (c == 0xED) {
            len = 3; hi = 0x9F;
        } else if (c == 0xF0) {
            len = 4; lo = 0x90;
        } else if (c >= 0xF1 && c <= 0xF3) {
            len = 4;
        } else if (c == 0xF4) {
            len = 4; hi = 0x8F;
        } else {
            out += kReplacement;
            ++i;
            continue;
        }
        // Consume the longest valid prefix; a truncated sequence is replaced
        // by one U+FFFD as a whole.
        std::size_t j = 1;
        bool ok = true;
        for (; j < len; ++j) {
            if (i + j >= n) { ok = false; break; }
            const unsigned char b = static_cast<unsigned char>(bytes[i + j]);
            const unsigned char min = j == 1 ? lo : 0x80;
            const unsigned char max = j == 1 ? hi : 0xBF;
            if (b < min || b > max) { ok = false; break; }
        }
        if (ok) {
            out.append(bytes, i, len);
            i += len;
        } else {
            out += kReplacement;
            i += j;
        }
    }
    return out;
}

void BuildLogger::Output(const std::string* step, bool is_stderr,
                         const std::string& bytes) const {
    if (!sink_) return;
    const std::string text = FromUtf8Lossy(bytes);
    const LogLevel level = is_stderr ? LogLevel::Warn : LogLevel::Info;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        const std::size_t next = end == std::string::npos ? text.size() : end + 1;
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        sink_->Log(level, step, line);
        start = next;
    }
}

}  // namespace core
}  // namespace agentenv
