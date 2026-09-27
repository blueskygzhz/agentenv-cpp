// SPDX-License-Identifier: MIT
// Rust: `tracing` + `tracing-subscriber`. C++11: structured, level-based logging
// with a global sink. Keep zero deps; format is deliberately terse.
#ifndef AGENTENV_CORE_LOGGING_H_
#define AGENTENV_CORE_LOGGING_H_

#include <sstream>
#include <string>

namespace agentenv {
namespace core {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error };

class Logger {
 public:
    static Logger& Get();

    void SetLevel(LogLevel lv);
    LogLevel Level() const;

    void Log(LogLevel lv, const char* file, int line, const std::string& msg);

 private:
    Logger();
    LogLevel level_;
};

}  // namespace core
}  // namespace agentenv

#define AGENTENV_LOG(lv, ...)                                                            \
    do {                                                                                 \
        auto& _lg = ::agentenv::core::Logger::Get();                                     \
        if (static_cast<int>(_lg.Level()) <= static_cast<int>(lv)) {                     \
            std::ostringstream _oss;                                                     \
            _oss << __VA_ARGS__;                                                         \
            _lg.Log(lv, __FILE__, __LINE__, _oss.str());                                 \
        }                                                                                \
    } while (0)

#define AGENTENV_TRACE(...) AGENTENV_LOG(::agentenv::core::LogLevel::Trace, __VA_ARGS__)
#define AGENTENV_DEBUG(...) AGENTENV_LOG(::agentenv::core::LogLevel::Debug, __VA_ARGS__)
#define AGENTENV_INFO(...)  AGENTENV_LOG(::agentenv::core::LogLevel::Info,  __VA_ARGS__)
#define AGENTENV_WARN(...)  AGENTENV_LOG(::agentenv::core::LogLevel::Warn,  __VA_ARGS__)
#define AGENTENV_ERROR(...) AGENTENV_LOG(::agentenv::core::LogLevel::Error, __VA_ARGS__)

#endif  // AGENTENV_CORE_LOGGING_H_
